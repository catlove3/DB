#include <db/catalog/catalog.h>
#include <db/common/error.h>
#include <db/storage/file_manager.h>
#include <db/storage/buffer_pool.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>
#include <unistd.h>

namespace db
{

using json = nlohmann::json;
using path = std::filesystem::path;

struct database_manager::opened_table
{
    table_schema schema;
    file_id fid = 0;
    std::unique_ptr<record_file> records;
    std::unique_ptr<row_codec> codec;
    std::size_t lease_count = 0;
};

// ---------- Catalog serialization and filesystem helpers ----------
namespace
{
    void require_format(bool condition,const char* message)
    {
        if(!condition)
            throw db_error(error_code::invalid_format,message);
    }

    // Convert only JSON/filesystem errors. Allocation failures must propagate.
    template<class F>
    auto catalog_call(F&& operation) -> decltype(operation())
    {
        try
        {
            return operation();
        }
        catch(const json::exception& error)
        {
            throw db_error(error_code::invalid_format,error.what());
        }
        catch(const std::filesystem::filesystem_error& error)
        {
            throw db_error(error_code::io_error,error.what());
        }
    }

    template<class T>
    T get_unsigned(const json& j)
    {
        require_format(j.is_number_integer(),"expected an unsigned integer");
        if(!j.is_number_unsigned())
            require_format(j.get<std::int64_t>() >= 0,"negative unsigned integer");
        std::uint64_t number = j.get<std::uint64_t>();
        require_format(number <= std::numeric_limits<T>::max(),"integer is out of range");
        return static_cast<T>(number);
    }

    std::int32_t get_integer(const json& j)
    {
        require_format(j.is_number_integer(),"INT default must be an integer");
        if(j.is_number_unsigned())
        {
            auto number = j.get<std::uint64_t>();
            require_format(number <= std::numeric_limits<std::int32_t>::max(),"INT default is out of range");
            return static_cast<std::int32_t>(number);
        }
        auto number = j.get<std::int64_t>();
        require_format(number >= std::numeric_limits<std::int32_t>::min() &&
                       number <= std::numeric_limits<std::int32_t>::max(),"INT default is out of range");
        return static_cast<std::int32_t>(number);
    }

    void check_name(const std::string& name)
    {
        require_format(!name.empty() && name.find('\0') == std::string::npos,"invalid catalog name");
    }

    template<class T>
    T get_enum(const json& j,std::initializer_list<std::pair<const char*,T>> names)
    {
        std::string name = j.get<std::string>();
        for(const auto& entry : names)
            if(name == entry.first)
                return entry.second;
        throw db_error(error_code::invalid_format,"unknown catalog enum value: " + name);
    }

    data_type get_type(const json& j)
    {
        return get_enum<data_type>(j,{{"INT",data_type::INT},{"FLOAT",data_type::FLOAT},
                                     {"VARCHAR",data_type::VARCHAR}});
    }

    template<class T>
    T get_schema(const json& j);

    template<class T>
    void get_schema_vector(const json& j,std::vector<T>& schemas)
    {
        require_format(j.is_array(),"schema list must be an array");
        schemas.clear();
        schemas.reserve(j.size());
        for(const json& item : j)
            schemas.push_back(get_schema<T>(item));
    }

    template<class T>
    std::optional<T> get_schema_nullable(const json& j)
    {
        if(j.is_null())
            return std::nullopt;
        return get_schema<T>(j);
    }

    std::vector<column_id> get_columns(const json& j)
    {
        require_format(j.is_array(),"column IDs must be an array");
        std::vector<column_id> columns;
        columns.reserve(j.size());
        for(const json& item : j)
            columns.push_back(get_unsigned<column_id>(item));
        return columns;
    }

    value get_default(const json& j)
    {
        std::string type = j.at("type").get<std::string>();
        const json& data = j.at("value");
        if(type == "NULL")
        {
            require_format(data.is_null(),"NULL default must contain JSON null");
            return std::monostate{};
        }
        switch(get_type(j.at("type")))
        {
            case data_type::INT:
                return get_integer(data);
            case data_type::FLOAT:
            {
                require_format(data.is_number(),"FLOAT default must be numeric");
                double number = data.get<double>();
                require_format(std::isfinite(number),"FLOAT default must be finite");
                return number;
            }
            case data_type::VARCHAR:
                return data.get<std::string>();
        }
        throw db_error(error_code::invalid_format,"unknown default value type");
    }

    template<>
    column_schema get_schema<column_schema>(const json& j)
    {
        column_schema column;
        column.id = get_unsigned<column_id>(j.at("id"));
        column.name = j.at("name").get<std::string>();
        column.ordinal = get_unsigned<std::uint32_t>(j.at("ordinal"));
        column.max_length = get_unsigned<std::uint32_t>(j.at("max_length"));
        column.declared_nullable = j.at("declared_nullable").get<bool>();
        column.has_default = j.at("has_default").get<bool>();

        column.type = get_type(j.at("type"));

        // The writer emits default_value; a column without DEFAULT may omit it.
        if(column.has_default || j.contains("default_value"))
            column.default_value = get_default(j.at("default_value"));
        if(!column.has_default)
            require_format(std::holds_alternative<std::monostate>(column.default_value),
                           "column without DEFAULT contains a default value");
        return column;
    }

    template<>
    foreign_key_schema get_schema<foreign_key_schema>(const json& j)
    {
        foreign_key_schema ret;
        ret.child_columns = get_columns(j.at("child_columns"));
        ret.child_index = get_unsigned<index_id>(j.at("child_index"));
        ret.id = get_unsigned<constraint_id>(j.at("id"));
        ret.name = j.at("name").get<std::string>();
        ret.parent_columns = get_columns(j.at("parent_columns"));
        ret.parent_table = get_unsigned<table_id>(j.at("parent_table"));
        return ret;
    }

    template<>
    index_schema get_schema<index_schema>(const json& j)
    {
        index_schema ret;
        ret.columns = get_columns(j.at("columns"));
        ret.file_path = j.at("file_path").get<std::string>();
        ret.id = get_unsigned<index_id>(j.at("id"));
        ret.name = j.at("name").get<std::string>();
        ret.null_policy = get_enum<index_null_policy>(j.at("null_policy"),{{"skip_nulls",index_null_policy::skip_nulls}});
        ret.origin = get_enum<index_origin>(j.at("origin"),{
            {"explicit_index",index_origin::explicit_index},{"primary_key",index_origin::primary_key},
            {"foreign_key",index_origin::foreign_key},{"unique",index_origin::unique}
        });
        ret.state = get_enum<index_state>(j.at("state"),{
            {"building",index_state::building},{"ready",index_state::ready}
        });
        return ret;
    }

    template<>
    primary_key_schema get_schema<primary_key_schema>(const json& j)
    {
        primary_key_schema ret;
        ret.columns = get_columns(j.at("columns"));
        ret.id = get_unsigned<constraint_id>(j.at("id"));
        ret.index = get_unsigned<index_id>(j.at("index"));
        ret.name = j.at("name").get<std::string>();
        return ret;
    }

    template<>
    table_schema get_schema<table_schema>(const json& j)
    {
        table_schema ret;
        get_schema_vector(j.at("columns"),ret.columns);
        get_schema_vector(j.at("foreign_keys"),ret.foreign_keys);
        ret.heap_path = j.at("heap_path").get<std::string>();
        ret.id = get_unsigned<table_id>(j.at("id"));
        get_schema_vector(j.at("indexes"),ret.indexes);
        ret.name = j.at("name").get<std::string>();
        ret.primary_key = get_schema_nullable<primary_key_schema>(j.at("primary_key"));
        ret.record_format_version = get_unsigned<std::uint32_t>(j.at("record_format_version"));
        return ret;
    }

    template<>
    root_catalog get_schema<root_catalog>(const json& j)
    {
        root_catalog ret;
        ret.format_version = get_unsigned<std::uint32_t>(j.at("format_version"));
        ret.next_database_id = get_unsigned<database_id>(j.at("next_database_id"));
        const json& databases = j.at("databases");
        require_format(databases.is_array(),"database list must be an array");
        for(const json& item : databases)
        {
            database_info info;
            info.id = get_unsigned<database_id>(item.at("id"));
            info.name = item.at("name").get<std::string>();
            ret.databases.push_back(std::move(info));
        }
        return ret;
    }

    template<>
    database_catalog get_schema<database_catalog>(const json& j)
    {
        database_catalog ret;
        ret.format_version = get_unsigned<std::uint32_t>(j.at("format_version"));
        ret.id = get_unsigned<database_id>(j.at("id"));
        ret.next_object_id = get_unsigned<std::uint64_t>(j.at("next_object_id"));
        get_schema_vector(j.at("tables"),ret.tables);
        return ret;
    }

    const char* type_name(data_type type)
    {
        switch(type)
        {
            case data_type::INT: return "INT";
            case data_type::FLOAT: return "FLOAT";
            case data_type::VARCHAR: return "VARCHAR";
        }
        throw db_error(error_code::invalid_format,"unknown column type");
    }

    json put_default(const value& data)
    {
        if(std::holds_alternative<std::monostate>(data))
            return {{"type","NULL"},{"value",nullptr}};
        if(std::holds_alternative<std::int32_t>(data))
            return {{"type","INT"},{"value",std::get<std::int32_t>(data)}};
        if(std::holds_alternative<double>(data))
        {
            double number = std::get<double>(data);
            require_format(std::isfinite(number),"FLOAT default must be finite");
            return {{"type","FLOAT"},{"value",number}};
        }
        require_format(std::holds_alternative<std::string>(data),"invalid default variant");
        return {{"type","VARCHAR"},{"value",std::get<std::string>(data)}};
    }

    json put_schema(const column_schema& column)
    {
        return {{"id",column.id},{"name",column.name},{"ordinal",column.ordinal},
                {"type",type_name(column.type)},{"max_length",column.max_length},
                {"declared_nullable",column.declared_nullable},{"has_default",column.has_default},
                {"default_value",put_default(column.default_value)}};
    }

    json put_schema(const foreign_key_schema& key)
    {
        return {{"id",key.id},{"name",key.name},{"child_columns",key.child_columns},
                {"parent_table",key.parent_table},{"parent_columns",key.parent_columns},
                {"child_index",key.child_index}};
    }

    json put_schema(const primary_key_schema& key)
    {
        return {{"id",key.id},{"name",key.name},{"columns",key.columns},{"index",key.index}};
    }

    json put_schema(const index_schema& index)
    {
        const char* origin = nullptr;
        switch(index.origin)
        {
            case index_origin::explicit_index: origin = "explicit_index"; break;
            case index_origin::primary_key: origin = "primary_key"; break;
            case index_origin::foreign_key: origin = "foreign_key"; break;
            case index_origin::unique: origin = "unique"; break;
        }
        require_format(origin != nullptr,"unknown index origin");
        require_format(index.null_policy == index_null_policy::skip_nulls,"unknown index null policy");
        require_format(index.state == index_state::building || index.state == index_state::ready,
                       "unknown index state");
        return {{"id",index.id},{"name",index.name},{"columns",index.columns},
                {"origin",origin},{"null_policy","skip_nulls"},
                {"state",index.state == index_state::ready?"ready":"building"},
                {"file_path",index.file_path.generic_string()}};
    }

    json put_schema(const table_schema& table)
    {
        json j;
        j["id"] = table.id;
        j["name"] = table.name;
        j["record_format_version"] = table.record_format_version;
        j["heap_path"] = table.heap_path.generic_string();
        j["columns"] = json::array();
        for(const auto& column : table.columns)
            j["columns"].push_back(put_schema(column));
        j["indexes"] = json::array();
        for(const auto& index : table.indexes)
            j["indexes"].push_back(put_schema(index));
        j["foreign_keys"] = json::array();
        for(const auto& key : table.foreign_keys)
            j["foreign_keys"].push_back(put_schema(key));
        j["primary_key"] = table.primary_key?put_schema(*table.primary_key):json(nullptr);
        return j;
    }

    json put_schema(const root_catalog& root)
    {
        json j;
        j["format_version"] = root.format_version;
        j["next_database_id"] = root.next_database_id;
        j["databases"] = json::array();
        for(const auto& info : root.databases)
            j["databases"].push_back({{"id",info.id},{"name",info.name}});
        return j;
    }

    json put_schema(const database_catalog& database)
    {
        json j;
        j["format_version"] = database.format_version;
        j["id"] = database.id;
        j["next_object_id"] = database.next_object_id;
        j["tables"] = json::array();
        for(const auto& table : database.tables)
            j["tables"].push_back(put_schema(table));
        return j;
    }

    void check_root(const root_catalog& root)
    {
        require_format(root.format_version == catalog_format_version,"unsupported root catalog version");
        require_format(root.next_database_id != 0,"invalid next database ID");
        std::set<database_id> ids;
        std::set<std::string> names;
        for(const auto& info : root.databases)
        {
            check_name(info.name);
            require_format(info.id != 0 && info.id < root.next_database_id,"invalid database ID allocator");
            require_format(ids.insert(info.id).second && names.insert(info.name).second,
                           "duplicate database ID or name");
        }
    }

    // Reject traversal and symlinks that resolve outside the owning directory.
    path checked_path(const path& base,const path& relative)
    {
        require_format(!relative.empty() && !relative.is_absolute() && !relative.has_root_path(),
                       "catalog path must be relative");
        require_format(relative.native().find('\0') == std::string::npos,"NUL in catalog path");
        for(const auto& part : relative)
            require_format(part != ".." && part != ".","invalid catalog path component");
        path root = std::filesystem::weakly_canonical(base);
        path resolved = std::filesystem::weakly_canonical(base / relative);
        auto match = std::mismatch(root.begin(),root.end(),resolved.begin(),resolved.end());
        require_format(match.first == root.end() && match.second != resolved.end(),
                       "catalog path escapes its owning directory");
        return resolved;
    }

    void check_columns(const table_schema& table,const std::vector<column_id>& columns)
    {
        require_format(!columns.empty(),"empty index or constraint key");
        std::set<column_id> seen;
        for(column_id id : columns)
        {
            auto found = std::find_if(table.columns.begin(),table.columns.end(),
                                     [&](const auto& column){return column.id == id;});
            require_format(found != table.columns.end() && seen.insert(id).second,
                           "unknown or duplicate column in key");
        }
    }

    const index_schema& check_index(const table_schema& table,index_id id,
                                   const std::vector<column_id>& columns,index_origin origin)
    {
        auto found = std::find_if(table.indexes.begin(),table.indexes.end(),
                                 [&](const auto& index){return index.id == id;});
        require_format(found != table.indexes.end(),"constraint references a missing index");
        require_format(found->columns == columns && found->origin == origin && found->state == index_state::ready,
                       "constraint index definition does not match");
        return *found;
    }

    void check_database(const database_catalog& database,const path& directory)
    {
        require_format(database.format_version == catalog_format_version,"unsupported database catalog version");
        require_format(database.id != 0 && database.next_object_id != 0,"invalid database ID or allocator");
        std::set<std::uint64_t> ids;
        std::set<std::string> table_names;
        std::set<path> files;
        auto add_id = [&](std::uint64_t id)
        {
            require_format(id != 0 && id < database.next_object_id && ids.insert(id).second,
                           "invalid or duplicate object ID");
        };
        auto add_file = [&](const path& relative)
        {
            path resolved = checked_path(directory,relative);
            require_format(files.insert(resolved).second,"duplicate catalog file path");
            require_format(std::filesystem::is_regular_file(resolved),"registered data file is missing or not regular");
        };

        for(const auto& table : database.tables)
        {
            add_id(table.id);
            check_name(table.name);
            require_format(table_names.insert(table.name).second,"duplicate table name");
            require_format(table.record_format_version == 1,"unsupported record format version");
            add_file(table.heap_path);
            std::set<column_id> column_ids;
            std::set<std::string> column_names;
            std::vector<column_spec> columns;
            for(std::size_t i = 0;i < table.columns.size();++i)
            {
                const auto& column = table.columns[i];
                check_name(column.name);
                require_format(column.id != 0 && column_ids.insert(column.id).second &&
                               column_names.insert(column.name).second,"invalid or duplicate column");
                require_format(column.ordinal == i,"column ordinal does not match its position");
                bool nullable = column.declared_nullable;
                if(table.primary_key && std::find(table.primary_key->columns.begin(),table.primary_key->columns.end(),
                                                 column.id) != table.primary_key->columns.end())
                    nullable = false;
                columns.push_back({column.type,column.max_length,nullable});
                if(!column.has_default)
                    require_format(std::holds_alternative<std::monostate>(column.default_value),
                                   "unexpected default value without DEFAULT");
                else
                {
                    // Reuse the existing codec's type, NULL, length and finite-value checks.
                    try
                    {
                        row_codec codec({columns.back()});
                        codec.encode({column.default_value});
                    }
                    catch(const db_error& error)
                    {
                        if(error.code() == error_code::feature_unsupported)
                            throw;
                        throw db_error(error_code::invalid_format,error.what());
                    }
                }
            }
            try
            {
                row_codec codec(columns);
            }
            catch(const db_error& error)
            {
                if(error.code() == error_code::feature_unsupported)
                    throw;
                throw db_error(error_code::invalid_format,error.what());
            }
            std::set<std::string> index_names;
            for(const auto& index : table.indexes)
            {
                add_id(index.id);
                check_name(index.name);
                require_format(index_names.insert(index.name).second,"duplicate index name");
                check_columns(table,index.columns);
                add_file(index.file_path);
            }
            std::set<std::string> constraint_names;
            if(table.primary_key)
            {
                const auto& key = *table.primary_key;
                add_id(key.id);
                check_name(key.name);
                constraint_names.insert(key.name);
                check_columns(table,key.columns);
                check_index(table,key.index,key.columns,index_origin::primary_key);
            }
            for(const auto& key : table.foreign_keys)
            {
                add_id(key.id);
                check_name(key.name);
                require_format(constraint_names.insert(key.name).second,"duplicate constraint name");
                check_columns(table,key.child_columns);
                check_index(table,key.child_index,key.child_columns,index_origin::foreign_key);
                auto parent = std::find_if(database.tables.begin(),database.tables.end(),
                                          [&](const auto& item){return item.id == key.parent_table;});
                require_format(parent != database.tables.end(),"foreign key references a missing table");
                check_columns(*parent,key.parent_columns);
                require_format(parent->primary_key && parent->primary_key->columns == key.parent_columns &&
                               key.child_columns.size() == key.parent_columns.size(),"foreign key must reference the parent primary key");
                for(std::size_t i = 0;i < key.child_columns.size();++i)
                {
                    auto child_column = std::find_if(table.columns.begin(),table.columns.end(),
                        [&](const auto& column){return column.id == key.child_columns[i];});
                    auto parent_column = std::find_if(parent->columns.begin(),parent->columns.end(),
                        [&](const auto& column){return column.id == key.parent_columns[i];});
                    require_format(child_column->type == parent_column->type && child_column->max_length == parent_column->max_length,
                                   "foreign key column types do not match");
                }
            }
        }
    }

    json read_json(const path& file,error_code missing)
    {
        if(!std::filesystem::exists(file))
            throw db_error(missing,"catalog file does not exist");
        require_format(std::filesystem::is_regular_file(file),"catalog path is not a regular file");
        std::ifstream in(file);
        if(!in)
            throw db_error(error_code::io_error,"failed to open catalog file");
        try
        {
            json j = json::parse(in);
            if(in.bad())
                throw db_error(error_code::io_error,"failed to read catalog file");
            return j;
        }
        catch(const json::exception&)
        {
            if(in.bad())
                throw db_error(error_code::io_error,"failed to read catalog file");
            throw;
        }
    }

    // The caller serializes writers. A successful rename is the publication point.
    void write_json(const path& file,const json& j)
    {
        std::string text = j.dump(json_indent) + "\n";
        std::string target = file.string();
        std::string pattern = target + ".tmp.XXXXXX";
        std::vector<char> temporary(pattern.begin(),pattern.end());
        temporary.push_back('\0');
        error_context context;
        context.operation = "save_catalog";
        context.path = target;
        int fd = ::mkstemp(temporary.data());
        if(fd == -1)
        {
            context.os_error = errno;
            throw db_error(error_code::io_error,"failed to create temporary catalog",std::move(context));
        }
        fd_guard guard(fd);
        try
        {
            safe_write(guard.get(),text.data(),text.size(),0);
            if(::close(guard.release()) != 0)
            {
                context.os_error = errno;
                throw db_error(error_code::io_error,"failed to close temporary catalog",std::move(context));
            }
            if(::rename(temporary.data(),target.c_str()) != 0)
            {
                context.os_error = errno;
                throw db_error(error_code::io_error,"failed to replace catalog",std::move(context));
            }
            // No allocating, logging or other fallible work after publication.
        }
        catch(...)
        {
            ::unlink(temporary.data());
            throw;
        }
    }
}

/*
    CLASS : CATALOG STORE
    docs:
        Own the data-root path and persist catalog value snapshots as JSON.
        JSON types, conversions and filesystem helpers stay private to this file.
        Database/table creation and table leases are implemented separately by the manager.

    layout:
        <root>/engine.meta.json                     -> root_catalog
        <root>/databases/<database_id>/catalog.json -> database_catalog
        <root>/databases/<database_id>/tables/<table_id>.heap
        <root>/databases/<database_id>/indexes/<index_id>.bpt
        JSON field names match schema members; enum values use explicit string names.
        Defaults contain type/value, and an absent primary key is JSON null.

    methods:
        init_meta     : Create directories and an empty root catalog; reject an existing catalog.
        load_root     : Read and validate versions, database IDs, names and the ID allocator.
        load_database : Require a registered ID; validate schema, references and relative file paths.
        save_root     : Validate and serialize the root, then publish through a temporary file.
        save_database : Validate and serialize a database, then publish through a temporary file.
        data_root     : Return the owned absolute root path; do not perform I/O.

    errors:
        Missing initial catalog or unknown database -> object_not_found.
        Existing root during initialization -> object_exists; malformed catalog -> invalid_format.
        Filesystem failures -> io_error; allocation failures propagate without reclassification.
        Callers serialize writes. New database directories/data files must exist before saving.
        Writes use exclusive temporary creation, checked write/close and same-directory rename.
        Publication failures preserve the previous file; this is not a power-loss durability guarantee.
*/

catalog_store::catalog_store(std::filesystem::path data_root):data_lord(std::move(data_root))
{
    if(data_lord.empty())
        throw db_error(error_code::invalid_argument,"empty data root");
    catalog_call([&]{data_lord = std::filesystem::absolute(data_lord).lexically_normal();});
}

void catalog_store::init_meta()
{
    catalog_call([&]
    {
        path meta_json = data_lord / "engine.meta.json";
        auto status = std::filesystem::symlink_status(meta_json);
        if(std::filesystem::exists(status))
            throw db_error(error_code::object_exists,"root catalog already exists");
        std::filesystem::create_directories(data_lord);
        path databases = checked_path(data_lord,"databases");
        std::filesystem::create_directories(databases);
        write_json(meta_json,put_schema(root_catalog{}));
    });
}

root_catalog catalog_store::load_root() const
{
    return catalog_call([&]
    {
        path meta_json = checked_path(data_lord,"engine.meta.json");
        root_catalog ret = get_schema<root_catalog>(read_json(meta_json,error_code::object_not_found));
        check_root(ret);
        return ret;
    });
}

database_catalog catalog_store::load_database(database_id id) const
{
    return catalog_call([&]
    {
        root_catalog root = load_root();
        auto found = std::find_if(root.databases.begin(),root.databases.end(),
                                 [&](const auto& info){return info.id == id;});
        if(found == root.databases.end())
            throw db_error(error_code::object_not_found,"database ID is not registered");
        path directory = checked_path(data_lord,path("databases") / std::to_string(id));
        path file = checked_path(directory,"catalog.json");
        database_catalog ret = get_schema<database_catalog>(read_json(file,error_code::invalid_format));
        require_format(ret.id == id,"database catalog ID does not match its directory");
        check_database(ret,directory);
        return ret;
    });
}

void catalog_store::save_root(const root_catalog& snapshot)
{
    catalog_call([&]
    {
        check_root(snapshot);
        json j = put_schema(snapshot);
        path file = checked_path(data_lord,"engine.meta.json");
        write_json(file,j);
    });
}

void catalog_store::save_database(const database_catalog& snapshot)
{
    catalog_call([&]
    {
        path directory = checked_path(data_lord,path("databases") / std::to_string(snapshot.id));
        json j = put_schema(snapshot);
        // Use the same decoding checks for in-memory snapshots before publication.
        check_database(get_schema<database_catalog>(j),directory);
        path file = checked_path(directory,"catalog.json");
        write_json(file,j);
    });
}

const std::filesystem::path& catalog_store::data_root() const noexcept
{
    return data_lord;
}

/*
    CLASS : DATABASE MANAGER
    docs:
        Coordinate catalog publication, the current database and open heap registrations.
        Borrow the store and both storage managers; all three must outlive this object.
        Reuse one open-table registration per table and keep its address stable for leases.
        This is an interface scaffold; database and table operations are not implemented yet.

    methods:
        create_database : Prepare a new ID-based directory and publish the root; do not select it.
        use_database    : Prepare the target, reject active leases, close old tables and switch.
        list_databases  : Return ID/name snapshots sorted by name, then ID; no selection required.
        create_table    : Validate columns/defaults, initialize and close the heap, then publish schema.
        describe_table  : Return a complete table-schema copy for the current database.
        list_tables     : Return current-database ID/name snapshots in stable order.
        open_table      : Reuse or prepare records and codec, check equal row sizes, return a lease.
        drop_table      : Check references/leases, close resources, publish removal, then reclaim files.
        drop_database   : Apply the same ordering; clear selection when dropping the current database.
        current_database: Return the selected persistent ID, or nullopt when no database is selected.
        close           : Close registered heaps through bpm; reject outstanding leases.

    errors:
        Missing selection -> no_database; duplicate/missing names or IDs -> object_exists/object_not_found.
        Active leases -> resource_busy; unsupported index/constraint definitions -> feature_unsupported.
        Mutating methods receive caller-owned statement_context and mark metadata_published after publication.
        Publication and cleanup are separate phases; a cleanup error cannot undo a published catalog.
*/


// ---------- table_lease ----------

/*
    CLASS : TABLE LEASE
    docs:
        Borrow one registered open table from database_manager.
        The manager owns schema, codec and records; it must outlive all leases.
        Moving transfers one borrow; release and destruction only reduce the borrow count.
        Active leases block DROP, database switching and explicit closure of the table.
        These lifetime rules are the implementation contract; methods are not implemented yet.

    methods:
        records : Access the record_file used for CRUD and scans.
        codec   : Access the row codec built from the effective column definitions.
        schema  : Access the complete table definition while the lease remains valid.
        valid   : Report whether this lease still owns a borrow.
        release : Give up the borrow without closing files; repeated release is harmless.

    errors:
        Access through a released or moved lease -> invalid_argument.
        Accessor references cannot outlive the lease; the manager closes files explicitly.
*/


}
