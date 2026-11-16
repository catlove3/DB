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

void statement_context::set_state(
            statement_phase p,
			std::uint64_t c,
			std::optional<std::string> ip,
			std::optional<std::size_t> il
)
{
    phase = p;
    completed_rows = c;
    input_path = ip;
    input_line = il;
}





// ---------- Catalog serialization and filesystem helpers ----------
namespace
{
    void require_format(bool condition,const char* message)
    {
        if(!condition)
            throw db_error(error_code::invalid_format,message);
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
            std::uint64_t number = j.get<std::uint64_t>();
            require_format(number <= std::numeric_limits<std::int32_t>::max(),"INT default is out of range");
            return static_cast<std::int32_t>(number);
        }
        std::int64_t number = j.get<std::int64_t>();
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
        for(const std::pair<const char*,T>& entry : names)
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
        for(const column_schema& column : table.columns)
            j["columns"].push_back(put_schema(column));
        j["indexes"] = json::array();
        for(const index_schema& index : table.indexes)
            j["indexes"].push_back(put_schema(index));
        j["foreign_keys"] = json::array();
        for(const foreign_key_schema& key : table.foreign_keys)
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
        for(const database_info& info : root.databases)
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
        for(const table_schema& table : database.tables)
            j["tables"].push_back(put_schema(table));
        return j;
    }

    void check_root(const root_catalog& root)
    {
        require_format(root.format_version == catalog_format_version,"unsupported root catalog version");
        require_format(root.next_database_id != 0,"invalid next database ID");
        std::set<database_id> ids;
        std::set<std::string> names;
        for(const database_info& info : root.databases)
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
        for(const path& part : relative)
            require_format(part != ".." && part != ".","invalid catalog path component");
        path root = std::filesystem::weakly_canonical(base);
        path resolved = std::filesystem::weakly_canonical(base / relative);
        std::pair<path::iterator,path::iterator> match =
            std::mismatch(root.begin(),root.end(),resolved.begin(),resolved.end());
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
            std::vector<column_schema>::const_iterator found =
                std::find_if(table.columns.begin(),table.columns.end(),
                             [&](const column_schema& column){return column.id == id;});
            require_format(found != table.columns.end() && seen.insert(id).second,
                           "unknown or duplicate column in key");
        }
    }

    const index_schema& check_index(const table_schema& table,index_id id,
                                   const std::vector<column_id>& columns,index_origin origin)
    {
        std::vector<index_schema>::const_iterator found =
            std::find_if(table.indexes.begin(),table.indexes.end(),
                         [&](const index_schema& index){return index.id == id;});
        require_format(found != table.indexes.end(),"constraint references a missing index");
        require_format(found->columns == columns && found->origin == origin && found->state == index_state::ready,
                       "constraint index definition does not match");
        return *found;
    }

    void add_id(std::set<std::uint64_t>& ids,std::uint64_t id,std::uint64_t next_object_id)
    {
        require_format(id != 0 && id < next_object_id && ids.insert(id).second,
                       "invalid or duplicate object ID");
    }

    void add_file(std::set<path>& files,const path& directory,const path& relative)
    {
        path resolved = checked_path(directory,relative);
        require_format(files.insert(resolved).second,"duplicate catalog file path");
        require_format(std::filesystem::is_regular_file(resolved),"registered data file is missing or not regular");
    }

    void check_database(const database_catalog& database,const path& directory)
    {
        require_format(database.format_version == catalog_format_version,"unsupported database catalog version");
        require_format(database.id != 0 && database.id != invalid_database_id && database.next_object_id != 0,
                       "invalid database ID or allocator");
        std::set<std::uint64_t> ids;
        std::set<std::string> table_names;
        std::set<path> files;

        for(const table_schema& table : database.tables)
        {
            add_id(ids,table.id,database.next_object_id);
            check_name(table.name);
            require_format(table_names.insert(table.name).second,"duplicate table name");
            require_format(table.record_format_version == 1,"unsupported record format version");
            add_file(files,directory,table.heap_path);
            std::set<column_id> column_ids;
            std::set<std::string> column_names;
            std::vector<column_spec> columns;
            for(std::size_t i = 0;i < table.columns.size();++i)
            {
                const column_schema& column = table.columns[i];
                check_name(column.name);
                require_format(column.id != 0 && column.id != invalid_column_id && column_ids.insert(column.id).second &&
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
            for(const index_schema& index : table.indexes)
            {
                add_id(ids,index.id,database.next_object_id);
                check_name(index.name);
                require_format(index_names.insert(index.name).second,"duplicate index name");
                check_columns(table,index.columns);
                add_file(files,directory,index.file_path);
            }
            std::set<std::string> constraint_names;
            if(table.primary_key)
            {
                const primary_key_schema& key = *table.primary_key;
                add_id(ids,key.id,database.next_object_id);
                check_name(key.name);
                constraint_names.insert(key.name);
                check_columns(table,key.columns);
                check_index(table,key.index,key.columns,index_origin::primary_key);
            }
            for(const foreign_key_schema& key : table.foreign_keys)
            {
                add_id(ids,key.id,database.next_object_id);
                check_name(key.name);
                require_format(constraint_names.insert(key.name).second,"duplicate constraint name");
                check_columns(table,key.child_columns);
                check_index(table,key.child_index,key.child_columns,index_origin::foreign_key);
                std::vector<table_schema>::const_iterator parent =
                    std::find_if(database.tables.begin(),database.tables.end(),
                                 [&](const table_schema& item){return item.id == key.parent_table;});
                require_format(parent != database.tables.end(),"foreign key references a missing table");
                check_columns(*parent,key.parent_columns);
                require_format(parent->primary_key && parent->primary_key->columns == key.parent_columns &&
                               key.child_columns.size() == key.parent_columns.size(),"foreign key must reference the parent primary key");
                for(std::size_t i = 0;i < key.child_columns.size();++i)
                {
                    std::vector<column_schema>::const_iterator child_column =
                        std::find_if(table.columns.begin(),table.columns.end(),
                                     [&](const column_schema& column){return column.id == key.child_columns[i];});
                    std::vector<column_schema>::const_iterator parent_column =
                        std::find_if(parent->columns.begin(),parent->columns.end(),
                                     [&](const column_schema& column){return column.id == key.parent_columns[i];});
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
        in.exceptions(std::ios::badbit);
        return json::parse(in);
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



    void check_definition_name(const std::string& name)
    {
        if(name.empty() || name.find('\0') != std::string::npos)
            throw db_error(error_code::invalid_argument,"invalid schema name");
    }

    std::uint64_t allocate_object_id(std::uint64_t& next_object_id)
    {
        if(next_object_id == 0)
            throw db_error(error_code::invalid_argument,"invalid object ID allocator");
        if(next_object_id == std::numeric_limits<std::uint64_t>::max())
            throw db_error(error_code::resource_limit,"object IDs are exhausted");
        return next_object_id++;
    }

    std::vector<column_schema> from_definition(const std::vector<column_definition>& its)
    {
        if(its.empty())
            throw db_error(error_code::invalid_argument,"table must contain columns");
        if(its.size() >= invalid_column_id)
            throw db_error(error_code::resource_limit,"column IDs are exhausted");
        std::vector<column_schema> new_columns;
        new_columns.reserve(its.size());
        std::set<std::string> names;
        for(std::size_t i = 0;i < its.size();++i)
        {
            column_schema new_column;
            const column_definition & it = its[i];
            check_definition_name(it.name);
            if(!names.insert(it.name).second)
                throw db_error(error_code::object_exists,"duplicate column name");
            if(!it.has_default && !std::holds_alternative<std::monostate>(it.default_value))
                throw db_error(error_code::invalid_argument,"default value without DEFAULT");
            new_column.declared_nullable = it.nullable;
            new_column.default_value = it.default_value;
            new_column.has_default = it.has_default;
            new_column.id = static_cast<column_id>(i + 1);
            new_column.ordinal = static_cast<std::uint32_t>(i);
            new_column.max_length = it.max_length;
            new_column.type = it.type;
            new_column.name = it.name;

            new_columns.push_back(std::move(new_column));
        }

        return new_columns;
    }

    const column_schema& find_column(const table_schema& table,const std::string& name)
    {
        for(const column_schema& column : table.columns)
            if(column.name == name)
                return column;
        throw db_error(error_code::object_not_found,"column does not exist: " + name);
    }

    std::vector<column_id> resolve_columns(const table_schema& table,const std::vector<std::string>& names)
    {
        if(names.empty())
            throw db_error(error_code::invalid_argument,"empty constraint key");
        std::vector<column_id> columns;
        columns.reserve(names.size());
        std::set<column_id> seen;
        for(const std::string& name : names)
        {
            column_id id = find_column(table,name).id;
            if(!seen.insert(id).second)
                throw db_error(error_code::invalid_argument,"duplicate column in constraint key");
            columns.push_back(id);
        }
        return columns;
    }

    primary_key_schema from_definition(const primary_key_definition& it,const table_schema& table,
                                       constraint_id id,index_id index)
    {
        primary_key_schema key;
        key.id = id;
        key.name = it.name.value_or("pk_" + std::to_string(id));
        check_definition_name(key.name);
        key.columns = resolve_columns(table,it.columns);
        key.index = index;
        return key;
    }

    foreign_key_schema from_definition(
        const foreign_key_definition& it,
        const table_schema& child,
        const database_catalog& database,
        constraint_id id,
        index_id child_index
    )
    {
        foreign_key_schema key;
        key.id = id;
        key.name = it.name.value_or("fk_" + std::to_string(id));
        check_definition_name(key.name);
        key.child_index = child_index;
        key.child_columns = resolve_columns(child,it.child_columns);

        const table_schema* parent = nullptr;
        if(it.parent_table == child.name)
            parent = &child;
        else
        {
            for(const table_schema& table : database.tables)
            {
                if(table.name == it.parent_table)
                {
                    parent = &table;
                    break;
                }
            }
        }
        if(parent == nullptr)
            throw db_error(error_code::object_not_found,"foreign key parent table does not exist");
        key.parent_table = parent->id;
        key.parent_columns = resolve_columns(*parent,it.parent_columns);
        if(!parent->primary_key || parent->primary_key->columns != key.parent_columns ||
           key.child_columns.size() != key.parent_columns.size())
            throw db_error(error_code::foreign_key_violation,"foreign key must reference the parent primary key");

        for(std::size_t i = 0;i < it.child_columns.size();++i)
        {
            const column_schema& child_column = find_column(child,it.child_columns[i]);
            const column_schema& parent_column = find_column(*parent,it.parent_columns[i]);
            if(child_column.type != parent_column.type || child_column.max_length != parent_column.max_length)
                throw db_error(error_code::foreign_key_violation,"foreign key column types do not match");
        }
        return key;
    }

    index_schema make_constraint_index(index_id id,const std::vector<column_id>& columns,index_origin origin)
    {
        index_schema index;
        index.id = id;
        index.name = "idx_" + std::to_string(id);
        index.columns = columns;
        index.origin = origin;
        index.state = index_state::building;
        index.file_path = path("indexes") / (std::to_string(id) + ".bpt");
        return index;
    }

    std::vector<column_spec> column_specs(const table_schema& table)
    {
        std::vector<column_spec> columns;
        columns.reserve(table.columns.size());
        for(const column_schema& column : table.columns)
        {
            bool nullable = column.declared_nullable;
            if(table.primary_key)
                for(column_id id : table.primary_key->columns)
                    if(id == column.id)
                        nullable = false;
            columns.push_back({column.type,column.max_length,nullable});
        }
        return columns;
    }
}

/*
    FUNCTION : FROM DEFINITION
    docs:
        Build one table schema from a CREATE TABLE request without performing file I/O.
        Copy column definitions, assign local column IDs and resolve constraint names to IDs.
        Allocate distinct table, constraint and supporting-index IDs from one local counter.
        Resolve self-references against the new table after its primary key is defined.
        Validate key order, column types, defaults and the effective NOT NULL rule for primary keys.

    result:
        Return the schema and advance database.next_object_id only after all checks succeed.
        The caller appends the returned schema to its database snapshot.
        Supporting indexes remain building; their files and enforcement are not created here.

    errors:
        Missing referenced names -> object_not_found; duplicate names -> object_exists.
        Invalid definitions -> invalid_argument; incompatible foreign keys -> foreign_key_violation.
        ID exhaustion -> resource_limit; row_codec reports type, NULL and size errors directly.
        Failure leaves the supplied database unchanged.
*/
table_schema from_definition(const create_table_request& request,database_catalog& database)
{
    check_definition_name(request.name);
    for(const table_schema& table : database.tables)
        if(table.name == request.name)
            throw db_error(error_code::object_exists,"table name already exists");

    std::uint64_t next_object_id = database.next_object_id;
    table_schema table;
    table.id = allocate_object_id(next_object_id);
    table.name = request.name;
    table.heap_path = path("tables") / (std::to_string(table.id) + ".heap");
    table.columns = from_definition(request.columns);
    std::set<std::string> constraint_names;
    if(request.primary_key)
    {
        constraint_id id = allocate_object_id(next_object_id);
        index_id index = allocate_object_id(next_object_id);
        table.primary_key = from_definition(*request.primary_key,table,id,index);
        constraint_names.insert(table.primary_key->name);
        table.indexes.push_back(make_constraint_index(index,table.primary_key->columns,index_origin::primary_key));
    }
    table.foreign_keys.reserve(request.foreign_keys.size());
    for(const foreign_key_definition& definition : request.foreign_keys)
    {
        constraint_id id = allocate_object_id(next_object_id);
        index_id index = allocate_object_id(next_object_id);
        foreign_key_schema key = from_definition(definition,table,database,id,index);
        if(!constraint_names.insert(key.name).second)
            throw db_error(error_code::object_exists,"duplicate constraint name");
        table.indexes.push_back(make_constraint_index(index,key.child_columns,index_origin::foreign_key));
        table.foreign_keys.push_back(std::move(key));
    }

    std::vector<column_spec> columns = column_specs(table);
    row_codec codec(columns);
    for(std::size_t i = 0;i < table.columns.size();++i)
    {
        if(table.columns[i].has_default)
        {
            row_codec default_codec({columns[i]});
            default_codec.encode({table.columns[i].default_value});
        }
    }
    database.next_object_id = next_object_id;
    return table;
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

    json format:
        engine.meta.json:
        {
            "format_version": 1,
            "next_database_id": 2,
            "databases":
            [
                {"id": 1, "name": "school"}
            ]
        }

        databases/1/catalog.json:
        {
            "format_version": 1,
            "id": 1,
            "next_object_id": 7,
            "tables":
            [
                {
                    "id": 1,
                    "name": "class",
                    "record_format_version": 1,
                    "heap_path": "tables/1.heap",
                    "columns":
                    [
                        {
                            "id": 1,
                            "name": "id",
                            "ordinal": 0,
                            "type": "INT",
                            "max_length": 0,
                            "declared_nullable": false,
                            "has_default": false,
                            "default_value": {"type": "NULL", "value": null}
                        }
                    ],
                    "indexes":
                    [
                        {
                            "id": 3,
                            "name": "pk_class_index",
                            "columns": [1],
                            "origin": "primary_key",
                            "null_policy": "skip_nulls",
                            "state": "ready",
                            "file_path": "indexes/3.bpt"
                        }
                    ],
                    "primary_key":
                    {
                        "id": 4,
                        "name": "pk_class",
                        "columns": [1],
                        "index": 3
                    },
                    "foreign_keys": []
                },
                {
                    "id": 2,
                    "name": "student",
                    "record_format_version": 1,
                    "heap_path": "tables/2.heap",
                    "columns":
                    [
                        {
                            "id": 1,
                            "name": "class_id",
                            "ordinal": 0,
                            "type": "INT",
                            "max_length": 0,
                            "declared_nullable": true,
                            "has_default": true,
                            "default_value": {"type": "NULL", "value": null}
                        },
                        {
                            "id": 2,
                            "name": "name",
                            "ordinal": 1,
                            "type": "VARCHAR",
                            "max_length": 32,
                            "declared_nullable": false,
                            "has_default": true,
                            "default_value": {"type": "VARCHAR", "value": ""}
                        }
                    ],
                    "indexes":
                    [
                        {
                            "id": 5,
                            "name": "fk_class_index",
                            "columns": [1],
                            "origin": "foreign_key",
                            "null_policy": "skip_nulls",
                            "state": "ready",
                            "file_path": "indexes/5.bpt"
                        }
                    ],
                    "primary_key": null,
                    "foreign_keys":
                    [
                        {
                            "id": 6,
                            "name": "fk_student_class",
                            "child_columns": [1],
                            "parent_table": 1,
                            "parent_columns": [1],
                            "child_index": 5
                        }
                    ]
                }
            ]
        }

    field rules:
        format_version is the catalog format version; record_format_version describes heap records.
        Both versions are currently 1. JSON object field order is not significant.
        next_database_id is the next available database ID, not the database count.
        next_object_id allocates table, index and constraint IDs within one database.
        These object IDs share one namespace; column IDs are local to each table.
        IDs start at 1; ordinal starts at 0 and must match the column's array position.
        Each ID type reserves its maximum value as invalid; it is never an object's ID.
        An allocator at its maximum is exhausted and must not be incremented.
        ID gaps are allowed. Database and table arrays need not be sorted by ID.
        An empty root has next_database_id = 1 and databases = [].
        An empty database has its own id, next_object_id = 1 and tables = [].
        The database name lives in engine.meta.json; the database catalog stores its ID.

        Column type is "INT", "FLOAT" or "VARCHAR".
        max_length is the VARCHAR payload limit in bytes; use 0 for INT and FLOAT.
        declared_nullable preserves the column declaration; primary-key columns are effectively NOT NULL.
        has_default = false means no DEFAULT clause, even though the writer emits a NULL placeholder.
        has_default = true with {"type": "NULL", "value": null} means DEFAULT NULL.
        Other default examples are {"type": "INT", "value": 18},
        {"type": "FLOAT", "value": 1.5} and {"type": "VARCHAR", "value": "Alice"}.
        The type tag preserves the db::value alternative; an empty string is different from NULL.
        INT defaults fit int32_t; FLOAT defaults are finite doubles.
        A default must match the column's type, effective nullability and length limit.
        default_value may be omitted only when has_default is false; the writer always emits it.

        Key/index column arrays contain column IDs, not names or zero-based ordinals.
        Their order matters for composite keys. primary_key is an object or JSON null.
        indexes and foreign_keys are arrays; use [] when there are no entries.
        Index origin is "explicit_index", "primary_key", "foreign_key" or "unique".
        Index null_policy is "skip_nulls"; state is "building" or "ready".
        A primary key references its ready primary_key index through index.
        A foreign key references its ready foreign_key index through child_index.
        parent_table identifies a table in the same database; parent_columns must match its primary key.
        Child and parent columns correspond by position and must have matching types and lengths.

        heap_path and file_path are relative to the database directory, not the working directory.
        Referenced files must already exist; serializing a schema does not create heap or index files.
        The example documents persisted metadata, including index/constraint definitions;
        it does not imply that the manager's index/constraint operations are implemented.

    methods:
        init_meta     : Create directories and an empty root catalog; reject an existing catalog.
        load_root     : Read and validate versions, database IDs, names and the ID allocator.
        load_database : Require a registered ID; validate schema, references and relative file paths.
        save_root     : Validate and serialize the root, then publish through a temporary file.
        save_database : Validate and serialize a database, then publish through a temporary file.
        data_root     : Return the owned absolute root path; do not perform I/O.

    errors:
        Missing initial catalog or unknown database -> object_not_found.
        Existing root during initialization -> object_exists; invalid database definitions -> invalid_format.
        JSON, filesystem and standard-library exceptions propagate unchanged.
        Failed stream opens and checked POSIX I/O operations report io_error.
        Callers serialize writes. New database directories/data files must exist before saving.
        Writes use exclusive temporary creation, checked write/close and same-directory rename.
        Publication failures preserve the previous file; this is not a power-loss durability guarantee.
*/

catalog_store::catalog_store(std::filesystem::path data_root):data_lord(std::move(data_root))
{
    if(data_lord.empty())
        throw db_error(error_code::invalid_argument,"empty data root");
    data_lord = std::filesystem::absolute(data_lord).lexically_normal();
}

void catalog_store::init_meta()
{
    path meta_json = data_lord / "engine.meta.json";
    std::filesystem::file_status status = std::filesystem::symlink_status(meta_json);
    if(std::filesystem::exists(status))
        throw db_error(error_code::object_exists,"root catalog already exists");
    std::filesystem::create_directories(data_lord);
    path databases = checked_path(data_lord,"databases");
    std::filesystem::create_directories(databases);
    write_json(meta_json,put_schema(root_catalog{}));
}

root_catalog catalog_store::load_root() const
{
    path meta_json = checked_path(data_lord,"engine.meta.json");
    root_catalog ret = get_schema<root_catalog>(read_json(meta_json,error_code::object_not_found));
    check_root(ret);
    return ret;
}

database_catalog catalog_store::load_database(database_id id) const
{
    root_catalog root = load_root();
    std::vector<database_info>::const_iterator found =
        std::find_if(root.databases.begin(),root.databases.end(),
                     [&](const database_info& info){return info.id == id;});
    if(found == root.databases.end())
        throw db_error(error_code::object_not_found,"database ID is not registered");
    path directory = checked_path(data_lord,path("databases") / std::to_string(id));
    path file = checked_path(directory,"catalog.json");
    database_catalog ret = get_schema<database_catalog>(read_json(file,error_code::invalid_format));
    require_format(ret.id == id,"database catalog ID does not match its directory");
    check_database(ret,directory);
    return ret;
}

void catalog_store::save_root(const root_catalog& snapshot)
{
    check_root(snapshot);
    json j = put_schema(snapshot);
    path file = checked_path(data_lord,"engine.meta.json");
    write_json(file,j);
}

void catalog_store::save_database(const database_catalog& snapshot)
{
    path directory = checked_path(data_lord,path("databases") / std::to_string(snapshot.id));
    json j = put_schema(snapshot);
    // Use the same decoding checks for in-memory snapshots before publication.
    check_database(get_schema<database_catalog>(j),directory);
    path file = checked_path(directory,"catalog.json");
    write_json(file,j);
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
        Manage database/table lifetimes and unconstrained heap creation.
        Constraint metadata can be converted, but constraint index creation/enforcement is not implemented.

    methods:
        create_database : Prepare a new ID-based directory and publish the root; do not select it.
        use_database    : Prepare the target, reject active leases, close old tables and switch.
        list_databases  : Return ID/name snapshots sorted by name, then ID; no selection required.
        create_table    : Validate columns/defaults, initialize and close the heap, then publish schema.
        describe_table  : Return a complete table-schema copy for the current database.
        list_tables     : Return current-database ID/name snapshots sorted by name, then ID.
        open_table      : Reuse or prepare records and codec, check equal row sizes, return a lease.
        drop_table      : Check references/leases, close resources, publish removal, then reclaim files.
        drop_database   : Apply the same ordering; clear selection when dropping the current database.
        current_database: Return the selected persistent ID, or nullopt when no database is selected.
        close           : Close registered heaps through bpm and clear selection; reject outstanding leases.

    helpers:
        find_database/find_table : Return an optional vector position; position 0 is a valid match.
        require_database         : Borrow the selected snapshot or report no_database.
        require_open_table       : Borrow an initialized registration or report object_not_found.
        release_table            : Decrease the borrow count without closing or performing I/O.
        close_table              : Flush/close an unused registration; retain a live handle on failure.

    errors:
        Missing selection -> no_database; duplicate/missing names or IDs -> object_exists/object_not_found.
        Active leases -> resource_busy; unsupported index/constraint definitions -> feature_unsupported.
        Mutating methods receive caller-owned statement_context and mark metadata_published after publication.
        Publication and cleanup are separate phases; a cleanup error cannot undo a published catalog.
        Failed switching retains the old selection, though some unused tables may already be closed.
        close() can be retried; after success the manager can select another database again.
        The caller must release every lease and dependent scan before destroying the manager.
*/


std::optional<std::size_t> database_manager::find_database(database_id id) const
{
    for(std::size_t i = 0;i < root_.databases.size();++i)
        if(root_.databases[i].id == id)
            return i;
    return std::nullopt;
}


std::optional<std::size_t> database_manager::find_database(const std::string & name) const
{
    for(std::size_t i = 0;i < root_.databases.size();++i)
        if(root_.databases[i].name == name)
            return i;
    return std::nullopt;
}




std::optional<std::size_t> database_manager::find_table(const create_table_request& request) const
{
    const database_catalog& database = require_database();
    for(std::size_t i = 0;i < database.tables.size();++i)
    {
        if(database.tables[i].name == request.name)
            return i;
    }
    return std::nullopt;
}

std::optional<std::size_t> database_manager::find_table(table_id id) const
{
    const database_catalog& database = require_database();
    for(std::size_t i = 0;i < database.tables.size();++i)
        if(database.tables[i].id == id)
            return i;
    return std::nullopt;
}

database_catalog& database_manager::require_database()
{
    if(!current_)
        throw db_error(error_code::no_database,"no database is selected");
    return *current_;
}

const database_catalog& database_manager::require_database() const
{
    if(!current_)
        throw db_error(error_code::no_database,"no database is selected");
    return *current_;
}

database_manager::opened_table& database_manager::require_open_table(table_id id)
{
    std::unordered_map<table_id,std::unique_ptr<opened_table>>::iterator found = open_tables_.find(id);
    if(found == open_tables_.end() || !found->second->records)
        throw db_error(error_code::object_not_found,"table is not open");
    return *found->second;
}

const database_manager::opened_table& database_manager::require_open_table(table_id id) const
{
    std::unordered_map<table_id,std::unique_ptr<opened_table>>::const_iterator found = open_tables_.find(id);
    if(found == open_tables_.end() || !found->second->records)
        throw db_error(error_code::object_not_found,"table is not open");
    return *found->second;
}

void database_manager::release_table(table_id id) noexcept
{
    std::unordered_map<table_id,std::unique_ptr<opened_table>>::iterator found = open_tables_.find(id);
    if(found != open_tables_.end() && found->second->lease_count != 0)
        --found->second->lease_count;
}

database_manager::database_manager(
                                catalog_store& store,
                                file_manager& fm,
                                buffer_pool_manager& bpm
                            ):
                            store_(store),
                            fm_(fm),
                            bpm_(bpm),
                            root_(store_.load_root()),
                            current_(std::nullopt)
{

}


database_manager::~database_manager() noexcept
{
    for(const std::pair<const table_id,std::unique_ptr<opened_table>>& it : open_tables_)
    {
        try
        {
            bpm_.close_file(it.second->fid);
        }
        catch(...)
        {
            std::fprintf(
                stderr,
                "[WARN] Failed to close file %llu during database manager destruction.\n",
                (unsigned long long)(it.second->fid)
            );
        }
    }
}


database_id database_manager::create_database(const std::string& name,statement_context& ctx)
{
    ctx.set_state(statement_phase::preparing,0,std::nullopt,std::nullopt);
    if(name.empty() || name.find('\0') != std::string::npos)
        throw db_error(error_code::invalid_argument,"invalid database name");

    std::optional<std::size_t> position = find_database(name);
    if(position)
    {
        throw db_error(
            error_code::object_exists,
            "attempting to create database with name that already exists"
            );
    }

    database_id next_id = root_.next_database_id;
    if(next_id == invalid_database_id)
    {
        throw db_error(
            error_code::resource_limit,
            "database IDs are exhausted"
        );
    }

    root_catalog updated_root = root_;
    updated_root.databases.push_back({next_id,name});
    updated_root.next_database_id = next_id + 1;
    database_catalog empty_catalog;
    empty_catalog.id = next_id;
    path directory = checked_path(store_.data_root(),path("databases") / std::to_string(next_id));
    if(!std::filesystem::create_directory(directory))
        throw db_error(error_code::object_exists,"database directory already exists");

    try
    {
        std::filesystem::create_directory(directory / "tables");
        std::filesystem::create_directory(directory / "indexes");
        store_.save_database(empty_catalog);
        store_.save_root(updated_root);
    }
    catch(...)
    {
        // Only remove the directory created by this unpublished operation.
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory,cleanup_error);
        if(cleanup_error)
            std::fprintf(stderr,"[WARN] Failed to clean up unpublished database %llu.\n",
                         static_cast<unsigned long long>(next_id));
        throw;
    }

    ctx.phase = statement_phase::metadata_published;
    root_ = std::move(updated_root);
    return next_id;
}

void database_manager::drop_database(database_id id,statement_context& ctx)
{
    ctx.set_state(statement_phase::preparing,0,std::nullopt,std::nullopt);
    root_catalog updated_root = root_;
    std::optional<std::size_t> position = find_database(id);

    if(!position)
    {
        throw db_error(
            error_code::object_not_found,
            "attempting to drop not-existing database"
        );
    }

    path directory = checked_path(store_.data_root(),path("databases") / std::to_string(id));
    updated_root.databases.erase(updated_root.databases.begin() + position.value());
    bool dropping_current = current_ && current_->id == id;
    if(dropping_current)
    {
        for(const std::pair<const table_id,std::unique_ptr<opened_table>>& it : open_tables_)
            if(it.second->lease_count != 0)
                throw db_error(error_code::resource_busy,"database still has active table leases");
        while(!open_tables_.empty())
            close_table(open_tables_.begin()->first);
    }

    // Publish removal before reclaiming files; a cleanup failure cannot undo it.
    store_.save_root(updated_root);
    ctx.phase = statement_phase::metadata_published;
    root_ = std::move(updated_root);
    if(dropping_current)
        current_.reset();
    std::filesystem::remove_all(directory);
}

void database_manager::close_table(table_id id)
{
    std::unordered_map<table_id,std::unique_ptr<opened_table>>::iterator found = open_tables_.find(id);
    if(found == open_tables_.end())
        throw db_error(error_code::object_not_found,"table is not open");
    if(found->second->lease_count != 0)
        throw db_error(error_code::resource_busy,"table still has active leases");

    try
    {
        bpm_.close_file(found->second->fid);
    }
    catch(...)
    {
        // A system close can report failure after the handle has been removed.
        // Keep the table for retry only while the file manager still owns it.
        try
        {
            fm_.get_file(found->second->fid);
        }
        catch(const db_error& error)
        {
            if(error.code() == error_code::invalid_file_handle)
                open_tables_.erase(found);
        }
        throw;
    }
    open_tables_.erase(found);
}



void database_manager::use_database(database_id id)
{
    if(!find_database(id))
        throw db_error(
            error_code::object_not_found,
            "attempting to use not existing database"
        );

    if(current_ && current_->id == id)
        return;

    database_catalog tmp = store_.load_database(id);
    for(const std::pair<const table_id,std::unique_ptr<opened_table>>& it : open_tables_)
    {
        if(it.second->lease_count != 0)
            throw db_error(
                error_code::resource_busy,
                "database still has active table leases"
            );
    }
    while(!open_tables_.empty())
        close_table(open_tables_.begin()->first);

    current_ = std::move(tmp);
}


std::vector<database_info> database_manager::list_databases() const
{
    std::vector<database_info> databases = root_.databases;
    std::sort(databases.begin(),databases.end(),[](const database_info& left,const database_info& right)
    {
        if(left.name != right.name)
            return left.name < right.name;
        return left.id < right.id;
    });
    return databases;
}

std::optional<database_id> database_manager::current_database() const noexcept
{
    if(!current_)
        return std::nullopt;
    return current_.value().id;
}

table_id database_manager::create_table(const create_table_request& request,statement_context& ctx)
{
    ctx.set_state(statement_phase::preparing,0,std::nullopt,std::nullopt);
    if(!current_)
        throw db_error(
            error_code::no_database,
            "Can't create table when no database is used"
        );

    std::optional<std::size_t> found = find_table(request);
    if(found != std::nullopt)
        throw db_error(
            error_code::object_exists,
            "attempting to create tables that has already exists"
        );

    if(request.primary_key || !request.foreign_keys.empty())
        throw db_error(error_code::feature_unsupported,"constraint indexes are not implemented yet");

    database_catalog updated_database = *current_;
    table_schema new_schema = from_definition(request,updated_database);
    table_id new_table_id = new_schema.id;
    row_codec codec(column_specs(new_schema));
    path directory = checked_path(store_.data_root(),path("databases") / std::to_string(updated_database.id));
    path file = checked_path(directory,new_schema.heap_path);
    std::string filename = file.string();
    updated_database.tables.push_back(std::move(new_schema));

    std::filesystem::create_directories(file.parent_path());
    fm_.create_file(filename);
    std::optional<file_id> fid;
    try
    {
        fid = fm_.open_file(filename);
        record_file::initialize(fm_,bpm_,*fid,codec.record_size());
        bpm_.close_file(*fid);
        fid.reset();
        store_.save_database(updated_database);
    }
    catch(...)
    {
        if(fid)
        {
            try
            {
                bpm_.close_file(*fid);
            }
            catch(const db_error& error)
            {
                // A failed system close may already have removed the handle.
                if(error.code() != error_code::invalid_file_handle)
                    std::fprintf(stderr,"[WARN] Failed to close unpublished table %llu.\n",
                                 static_cast<unsigned long long>(new_table_id));
            }
            catch(...)
            {
                std::fprintf(stderr,"[WARN] Failed to close unpublished table %llu.\n",
                             static_cast<unsigned long long>(new_table_id));
            }
        }
        try
        {
            // remove_file refuses to unlink a file that is still registered as open.
            fm_.remove_file(filename);
        }
        catch(...)
        {
            std::fprintf(stderr,"[WARN] Failed to remove unpublished table %llu.\n",
                         static_cast<unsigned long long>(new_table_id));
        }
        throw;
    }

    ctx.phase = statement_phase::metadata_published;
    current_ = std::move(updated_database);
    return new_table_id;
}

table_schema database_manager::describe_table(table_id id) const
{
    std::optional<std::size_t> position = find_table(id);
    if(!position)
        throw db_error(error_code::object_not_found,"table does not exist");
    return require_database().tables[*position];
}

std::vector<table_info> database_manager::list_tables() const
{
    const database_catalog& database = require_database();
    std::vector<table_info> tables;
    tables.reserve(database.tables.size());
    for(const table_schema& table : database.tables)
        tables.push_back({table.id,table.name});
    std::sort(tables.begin(),tables.end(),[](const table_info& left,const table_info& right)
    {
        if(left.name != right.name)
            return left.name < right.name;
        return left.id < right.id;
    });
    return tables;
}

table_lease database_manager::open_table(table_id id)
{
    const database_catalog& database = require_database();
    std::unordered_map<table_id,std::unique_ptr<opened_table>>::iterator found = open_tables_.find(id);
    if(found != open_tables_.end())
    {
        if(found->second->records)
        {
            if(found->second->lease_count == std::numeric_limits<std::size_t>::max())
                throw db_error(error_code::resource_limit,"table lease count is exhausted");
            ++found->second->lease_count;
            return table_lease(*this,id);
        }
        // Retry cleanup left by an earlier failed open before opening a fresh handle.
        close_table(id);
    }

    std::unique_ptr<opened_table> table = std::make_unique<opened_table>();
    table->schema = describe_table(id);
    table->codec = std::make_unique<row_codec>(column_specs(table->schema));
    path directory = checked_path(store_.data_root(),path("databases") / std::to_string(database.id));
    std::string filename = checked_path(directory,table->schema.heap_path).string();

    // Allocate the registry entry before acquiring the file; retain it if cleanup needs retry.
    opened_table& opened = *table;
    open_tables_.emplace(id,std::move(table));
    bool file_opened = false;
    try
    {
        opened.fid = fm_.open_file(filename);
        file_opened = true;
        opened.records = std::make_unique<record_file>(fm_,bpm_,opened.fid);
        if(opened.records->record_size() != opened.codec->record_size())
            throw db_error(error_code::invalid_format,"heap record size does not match table schema");
        opened.lease_count = 1;
    }
    catch(...)
    {
        opened.records.reset();
        if(file_opened)
        {
            try
            {
                close_table(id);
            }
            catch(...)
            {
                std::fprintf(stderr,"[WARN] Failed to close table %llu after opening failed.\n",
                             static_cast<unsigned long long>(id));
            }
        }
        else
            open_tables_.erase(id);
        throw;
    }
    return table_lease(*this,id);
}

void database_manager::drop_table(table_id id,statement_context& ctx)
{
    ctx.set_state(statement_phase::preparing,0,std::nullopt,std::nullopt);
    const database_catalog& database = require_database();
    std::optional<std::size_t> position = find_table(id);
    if(!position)
        throw db_error(error_code::object_not_found,"table does not exist");
    for(const table_schema& table : database.tables)
    {
        if(table.id == id)
            continue;
        for(const foreign_key_schema& key : table.foreign_keys)
            if(key.parent_table == id)
                throw db_error(error_code::dependency_exists,"table is referenced by a foreign key");
    }

    const table_schema& table = database.tables[*position];
    path directory = checked_path(store_.data_root(),path("databases") / std::to_string(database.id));
    std::vector<std::string> files;
    files.reserve(table.indexes.size() + 1);
    files.push_back(checked_path(directory,table.heap_path).string());
    for(const index_schema& index : table.indexes)
        files.push_back(checked_path(directory,index.file_path).string());
    database_catalog updated_database = database;
    updated_database.tables.erase(updated_database.tables.begin() + *position);
    if(open_tables_.find(id) != open_tables_.end())
        close_table(id);

    store_.save_database(updated_database);
    ctx.phase = statement_phase::metadata_published;
    current_ = std::move(updated_database);
    for(const std::string& file : files)
        fm_.remove_file(file);
}

void database_manager::close()
{
    // Check all borrows before closing any table, so a busy refusal changes nothing.
    for(const std::pair<const table_id,std::unique_ptr<opened_table>>& it : open_tables_)
        if(it.second->lease_count != 0)
            throw db_error(error_code::resource_busy,"database still has active table leases");
    while(!open_tables_.empty())
        close_table(open_tables_.begin()->first);
    current_.reset();
}

/*
    CLASS : TABLE LEASE
    docs:
        Borrow one registered open table from database_manager.
        The manager owns schema, codec and records; it must outlive all leases.
        Moving transfers one borrow; release and destruction only reduce the borrow count.
        Active leases block DROP, database switching and explicit closure of the table.
        Lease access does not enforce primary/foreign keys or maintain indexes; execution layers own that work.

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

table_lease::table_lease(database_manager& manager,table_id id) noexcept:manager_(&manager),table_(id)
{
}

table_lease::~table_lease() noexcept
{
    release();
}

table_lease::table_lease(table_lease&& other) noexcept:manager_(other.manager_),table_(other.table_)
{
    other.manager_ = nullptr;
    other.table_ = invalid_table_id;
}

table_lease& table_lease::operator=(table_lease&& other) noexcept
{
    if(this != &other)
    {
        release();
        manager_ = other.manager_;
        table_ = other.table_;
        other.manager_ = nullptr;
        other.table_ = invalid_table_id;
    }
    return *this;
}

record_file& table_lease::records() const
{
    if(!valid())
        throw db_error(error_code::invalid_argument,"table lease has been released or moved");
    return *manager_->require_open_table(table_).records;
}

const row_codec& table_lease::codec() const
{
    if(!valid())
        throw db_error(error_code::invalid_argument,"table lease has been released or moved");
    const database_manager& manager = *manager_;
    return *manager.require_open_table(table_).codec;
}

const table_schema& table_lease::schema() const
{
    if(!valid())
        throw db_error(error_code::invalid_argument,"table lease has been released or moved");
    const database_manager& manager = *manager_;
    return manager.require_open_table(table_).schema;
}

bool table_lease::valid() const noexcept
{
    return manager_ != nullptr;
}

void table_lease::release() noexcept
{
    if(!valid())
        return;
    manager_->release_table(table_);
    manager_ = nullptr;
    table_ = invalid_table_id;
}

}
