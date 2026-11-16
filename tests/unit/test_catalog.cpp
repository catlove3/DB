#include <db/catalog/catalog.h>
#include <db/common/error.h>
#include <db/storage/buffer_pool.h>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unistd.h>

using namespace db;
using json = nlohmann::json;
namespace fs = std::filesystem;

#ifdef IDBS_CATALOG_FAILURE_TESTS
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_close(int);
extern "C" int __real_rename(const char*,const char*);
extern "C" int __real_unlink(const char*);
namespace
{
    bool fail_write = false;
    bool fail_close = false;
    bool fail_rename = false;
    int fail_rename_after = -1;
    bool fail_unlink = false;
    bool short_write = false;
}
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t size,off_t offset)
{
    if(short_write)
    {
        short_write = false;
        fail_write = true;
        return __real_pwrite(fd,data,size > 1?size/2:size,offset);
    }
    if(fail_write)
    {
        fail_write = false;
        errno = EIO;
        return -1;
    }
    return __real_pwrite(fd,data,size,offset);
}
extern "C" int __wrap_close(int fd)
{
    int result = __real_close(fd);
    if(fail_close)
    {
        fail_close = false;
        errno = EIO;
        return -1;
    }
    return result;
}
extern "C" int __wrap_rename(const char* from,const char* to)
{
    if(fail_rename || fail_rename_after == 0)
    {
        fail_rename = false;
        fail_rename_after = -1;
        errno = EIO;
        return -1;
    }
    if(fail_rename_after > 0)
        --fail_rename_after;
    return __real_rename(from,to);
}
extern "C" int __wrap_unlink(const char* path)
{
    if(fail_unlink)
    {
        fail_unlink = false;
        errno = EIO;
        return -1;
    }
    return __real_unlink(path);
}
#endif

namespace
{
    int failures = 0;

#define CHECK(condition) \
    do { \
        if(!(condition)) { \
            std::cerr << __FILE__ << ":" << __LINE__ \
                      << ": CHECK failed: " #condition << '\n'; \
            ++failures; \
        } \
    } while(false)

    struct temp_dir
    {
        fs::path path;
        temp_dir()
        {
            std::string pattern = (fs::temp_directory_path() / "idbs_catalog_XXXXXX").string();
            char* created = ::mkdtemp(pattern.data());
            if(!created)
                throw std::runtime_error("mkdtemp failed");
            path = created;
        }
        ~temp_dir()
        {
            std::error_code ignored;
            fs::remove_all(path,ignored);
        }
    };

    template<class F>
    void expect_error(error_code code,F&& operation)
    {
        try
        {
            operation();
            CHECK(false);
        }
        catch(const db_error& error)
        {
            CHECK(error.code() == code);
        }
    }

    template<class Exception,class F>
    void expect_exception(F&& operation)
    {
        try
        {
            operation();
            CHECK(false);
        }
        catch(const Exception&)
        {
        }
    }

    std::string read_text(const fs::path& file)
    {
        std::ifstream in(file);
        if(!in)
            throw std::runtime_error("cannot read fixture");
        return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    }

    void write_text(const fs::path& file,const std::string& text)
    {
        std::ofstream out(file);
        out << text;
        out.close();
        if(!out)
            throw std::runtime_error("cannot write fixture");
    }

    column_schema column(column_id id,std::string name,data_type type,value default_value,bool has_default = true)
    {
        column_schema ret;
        ret.id = id;
        ret.name = std::move(name);
        ret.ordinal = id-1;
        ret.type = type;
        ret.max_length = type == data_type::VARCHAR?16:0;
        ret.has_default = has_default;
        ret.default_value = std::move(default_value);
        return ret;
    }

    index_schema index(index_id id,std::string name,column_id column,index_origin origin)
    {
        index_schema ret;
        ret.id = id;
        ret.name = std::move(name);
        ret.columns = {column};
        ret.origin = origin;
        ret.state = index_state::ready;
        ret.file_path = fs::path("indexes") / (std::to_string(id)+".bpt");
        return ret;
    }

    database_catalog make_database()
    {
        database_catalog database;
        database.id = 1;
        database.next_object_id = 9;
        table_schema parent;
        parent.id = 1;
        parent.name = "class";
        parent.heap_path = "tables/1.heap";
        parent.columns.push_back(column(1,"id",data_type::INT,std::monostate{},false));
        parent.indexes.push_back(index(3,"pk_class",1,index_origin::primary_key));
        parent.primary_key = primary_key_schema{4,"pk_class",{1},3};

        table_schema child;
        child.id = 2;
        child.name = "student";
        child.heap_path = "tables/2.heap";
        child.columns.push_back(column(1,"class_id",data_type::INT,std::monostate{},false));
        child.columns.push_back(column(2,"age",data_type::INT,std::numeric_limits<std::int32_t>::min()));
        child.columns.push_back(column(3,"score",data_type::FLOAT,-0.0));
        child.columns.push_back(column(4,"name",data_type::VARCHAR,std::string("A\0B",3)));
        child.columns.push_back(column(5,"note",data_type::VARCHAR,std::monostate{}));
        child.columns.push_back(column(6,"empty",data_type::VARCHAR,std::string{}));
        child.indexes.push_back(index(5,"fk_class",1,index_origin::foreign_key));
        child.indexes.push_back(index(7,"score",3,index_origin::explicit_index));
        child.indexes.push_back(index(8,"name_unique",4,index_origin::unique));
        child.indexes.back().state = index_state::building;
        child.foreign_keys.push_back(foreign_key_schema{6,"fk_class",{1},1,{1},5});
        database.tables = {child,parent};
        return database;
    }

    struct fixture
    {
        temp_dir tmp;
        catalog_store store;
        database_catalog database = make_database();
        fs::path root_file;
        fs::path database_file;
        fixture():store(tmp.path / "nested" / "data")
        {
            store.init_meta();
            root_file = store.data_root() / "engine.meta.json";
            fs::path directory = store.data_root() / "databases" / "1";
            database_file = directory / "catalog.json";
            fs::create_directories(directory / "tables");
            fs::create_directories(directory / "indexes");
            for(const auto& table : database.tables)
            {
                // Catalog checks file identity/existence; record/index layers validate their bytes.
                write_text(directory / table.heap_path,"");
                for(const auto& item : table.indexes)
                    write_text(directory / item.file_path,"");
            }
            store.save_database(database);
            auto root = store.load_root();
            root.databases.push_back({1,"school"});
            root.next_database_id = 2;
            store.save_root(root);
        }
    };

    void test_initialization()
    {
        temp_dir tmp;
        catalog_store store(tmp.path / "nested" / "data");
        expect_error(error_code::object_not_found,[&]{store.load_root();});
        store.init_meta();
        CHECK(fs::is_directory(store.data_root() / "databases"));
        auto root = store.load_root();
        CHECK(root.format_version == 1);
        CHECK(root.next_database_id == 1);
        CHECK(root.databases.empty());
        fs::path file = store.data_root() / "engine.meta.json";
        std::string before = read_text(file);
        expect_error(error_code::object_exists,[&]{store.init_meta();});
        CHECK(read_text(file) == before);
        expect_error(error_code::object_not_found,[&]{store.load_database(1);});
        expect_error(error_code::invalid_argument,[]{catalog_store empty("");});

        write_text(tmp.path / "not_directory","sentinel");
        catalog_store blocked(tmp.path / "not_directory" / "data");
        expect_exception<fs::filesystem_error>([&]{blocked.init_meta();});
        CHECK(read_text(tmp.path / "not_directory") == "sentinel");
        CHECK(data_dir.lexically_normal() == (fs::path(__FILE__).parent_path().parent_path().parent_path() / "data").lexically_normal());
    }

    void test_round_trip()
    {
        fixture f;
        catalog_store reopened(f.store.data_root());
        auto root = reopened.load_root();
        CHECK(root.databases.size() == 1 && root.databases[0].name == "school");
        auto loaded = reopened.load_database(1);
        CHECK(loaded.tables.size() == 2);
        const auto& child = loaded.tables[0];
        CHECK(!child.primary_key);
        CHECK(loaded.tables[1].primary_key->index == 3);
        CHECK(child.columns[2].type == data_type::FLOAT);
        CHECK(child.columns[3].type == data_type::VARCHAR);
        CHECK(std::get<std::int32_t>(child.columns[1].default_value) == std::numeric_limits<std::int32_t>::min());
        CHECK(std::signbit(std::get<double>(child.columns[2].default_value)));
        CHECK(std::get<std::string>(child.columns[3].default_value) == std::string("A\0B",3));
        CHECK(child.columns[4].has_default && std::holds_alternative<std::monostate>(child.columns[4].default_value));
        CHECK(!child.columns[0].has_default);
        CHECK(std::holds_alternative<std::string>(child.columns[5].default_value));
        CHECK(std::get<std::string>(child.columns[5].default_value).empty());
        CHECK(child.indexes[0].origin == index_origin::foreign_key);
        CHECK(child.indexes[1].origin == index_origin::explicit_index);
        CHECK(child.indexes[2].origin == index_origin::unique && child.indexes[2].state == index_state::building);
        CHECK(child.foreign_keys[0].parent_table == 1);
        CHECK(child.foreign_keys[0].child_columns == std::vector<column_id>{1});
        std::string before = read_text(f.database_file);
        reopened.save_database(loaded);
        CHECK(read_text(f.database_file) == before);
        json j = json::parse(before);
        CHECK(j.contains("format_version") && j.contains("next_object_id"));
        CHECK(j["tables"][0].contains("record_format_version"));
        CHECK(j["tables"][0]["primary_key"].is_null());
        CHECK(j["tables"][0]["indexes"][0].contains("origin"));
        j["tables"][0]["columns"][2]["default_value"]["value"] = 18;
        j["tables"][0]["columns"][0].erase("default_value");
        write_text(f.database_file,j.dump());
        auto changed = reopened.load_database(1);
        CHECK(std::get<double>(changed.tables[0].columns[2].default_value) == 18.0);
        CHECK(!changed.tables[0].columns[0].has_default);
    }

    void test_record_reopen()
    {
        temp_dir tmp;
        catalog_store store(tmp.path);
        store.init_meta();
        database_catalog database;
        database.id = 1;
        database.next_object_id = 2;
        table_schema table;
        table.id = 1;
        table.name = "student";
        table.heap_path = "tables/1.heap";
        table.columns = {
            column(1,"id",data_type::INT,std::monostate{},false),
            column(2,"name",data_type::VARCHAR,std::string("Alice"))
        };
        database.tables.push_back(table);
        fs::path directory = tmp.path / "databases" / "1";
        fs::create_directories(directory / "tables");
        const row values{std::int32_t{42},std::string("Alice")};
        rid record;
        {
            file_manager fm;
            buffer_pool_manager bpm(fm,3);
            row_codec codec({{data_type::INT,0,true},{data_type::VARCHAR,16,true}});
            std::string file = (directory / table.heap_path).string();
            fm.create_file(file);
            file_id fid = fm.open_file(file);
            record_file::initialize(fm,bpm,fid,codec.record_size());
            record_file records(fm,bpm,fid);
            record = records.insert(codec.encode(values));
            bpm.close_file(fid);
        }
        store.save_database(database);
        auto root = store.load_root();
        root.databases.push_back({1,"school"});
        root.next_database_id = 2;
        store.save_root(root);

        // Rebuild the codec solely from the persisted column definitions.
        catalog_store reopened(tmp.path);
        auto restored = reopened.load_database(1);
        const auto& schema = restored.tables[0];
        std::vector<column_spec> columns;
        for(const auto& item : schema.columns)
            columns.push_back({item.type,item.max_length,item.declared_nullable});
        row_codec codec(columns);
        file_manager fm;
        buffer_pool_manager bpm(fm,3);
        file_id fid = fm.open_file((directory / schema.heap_path).string());
        record_file records(fm,bpm,fid);
        CHECK(codec.decode(records.get(record)) == values);
        bpm.close_file(fid);
    }

    template<class Exception = db_error,class F>
    void corrupt(fixture& f,bool root,F&& change)
    {
        const fs::path& file = root?f.root_file:f.database_file;
        std::string before = read_text(file);
        json j = json::parse(before);
        change(j);
        write_text(file,j.dump());
        auto load = [&]
        {
            if(root)
                f.store.load_root();
            else
                f.store.load_database(1);
        };
        if constexpr(std::is_same_v<Exception,db_error>)
            expect_error(error_code::invalid_format,load);
        else
            expect_exception<Exception>(load);
        write_text(file,before);
    }

    void test_corrupt_catalogs()
    {
        fixture f;
        corrupt(f,true,[](json& j){j["format_version"] = 99;});
        corrupt(f,true,[](json& j){j["format_version"] = 4294967297ULL;});
        corrupt(f,true,[](json& j){j["next_database_id"] = -1;});
        corrupt(f,true,[](json& j){j["next_database_id"] = 1.5;});
        corrupt(f,true,[](json& j){j["next_database_id"] = 1;});
        corrupt(f,true,[](json& j){j["databases"] = json::object();});
        corrupt(f,true,[](json& j){j["databases"].push_back(j["databases"][0]);});
        corrupt(f,true,[](json& j){j["databases"][0]["id"] = 0;});
        corrupt(f,true,[](json& j){j["databases"][0]["id"] = invalid_database_id;});
        corrupt<json::out_of_range>(f,true,[](json& j){j.erase("next_database_id");});
        corrupt(f,false,[](json& j){j["id"] = 2;});
        corrupt(f,false,[](json& j){j["next_object_id"] = 8;});
        corrupt(f,false,[](json& j){j["tables"] = json::object();});
        corrupt(f,false,[](json& j){j["tables"][0]["id"] = invalid_table_id;});
        corrupt(f,false,[](json& j){j["tables"][0]["indexes"][0]["id"] = invalid_index_id;});
        corrupt(f,false,[](json& j){j["tables"][0]["foreign_keys"][0]["id"] = invalid_constraint_id;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["id"] = invalid_column_id;});
        corrupt(f,false,[](json& j){j["tables"][0]["record_format_version"] = 99;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["ordinal"] = -1;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["id"] = 4294967297ULL;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["id"] = 1;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["type"] = "UNKNOWN";});
        corrupt<json::type_error>(f,false,[](json& j){j["tables"][0]["columns"][1]["declared_nullable"] = 1;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["max_length"] = 1;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["default_value"]["value"] = 2147483648ULL;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["default_value"]["value"] = -2147483649LL;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["default_value"]["value"] = 1.5;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["default_value"]["type"] = "FLOAT";});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["has_default"] = false;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][2]["default_value"]["value"] = nullptr;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][3]["default_value"]["value"] = std::string(17,'x');});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][4]["default_value"]["value"] = 0;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][4]["declared_nullable"] = false;});
        corrupt<json::type_error>(f,false,[](json& j){j["tables"][0]["indexes"][0]["origin"] = 2;});
        corrupt(f,false,[](json& j){j["tables"][0]["indexes"][0]["origin"] = "oops";});
        corrupt(f,false,[](json& j){j["tables"][0]["indexes"][0]["state"] = "oops";});
        corrupt(f,false,[](json& j){j["tables"][0]["indexes"][0]["columns"] = json::object();});
        corrupt(f,false,[](json& j){j["tables"][0]["indexes"][0]["columns"] = {1,1};});
        corrupt(f,false,[](json& j){j["tables"][0]["indexes"][0]["columns"] = {-1};});
        corrupt(f,false,[](json& j){j["tables"][0]["foreign_keys"][0]["parent_table"] = 99;});
        corrupt(f,false,[](json& j){j["tables"][0]["foreign_keys"][0]["parent_columns"] = {99};});
        corrupt(f,false,[](json& j){j["tables"][1]["primary_key"]["index"] = 99;});
        corrupt(f,false,[](json& j){j["tables"][0]["heap_path"] = "../outside.heap";});
        corrupt(f,false,[](json& j){j["tables"][0]["heap_path"] = "/tmp/outside.heap";});
        corrupt(f,false,[](json& j){j["tables"][0]["heap_path"] = "tables/missing.heap";});
        corrupt(f,false,[](json& j){j["tables"][0]["heap_path"] = "tables";});
        corrupt(f,false,[](json& j){j["tables"][0]["heap_path"] = "tables/1.heap";});
        corrupt(f,false,[](json& j){j["tables"][0]["heap_path"] = std::string("tables/2.heap\0bad",17);});

        std::string before = read_text(f.database_file);
        for(const std::string& text : {std::string("{"),before + " trailing"})
        {
            write_text(f.database_file,text);
            expect_exception<json::parse_error>([&]{f.store.load_database(1);});
        }
        write_text(f.database_file,"null");
        expect_exception<json::type_error>([&]{f.store.load_database(1);});
        write_text(f.database_file,before);
        fs::remove(f.database_file);
        expect_error(error_code::invalid_format,[&]{f.store.load_database(1);});
        expect_error(error_code::object_not_found,[&]{f.store.load_database(999);});
    }

    void test_database_manager()
    {
        temp_dir tmp;
        catalog_store store(tmp.path);
        store.init_meta();
        file_manager fm;
        buffer_pool_manager bpm(fm,3);
        statement_context ctx;
        {
            database_manager manager(store,fm,bpm);
            ctx.set_state(statement_phase::between_rows,42,std::string("input.csv"),43);
            database_id first = manager.create_database("school",ctx);
            CHECK(first == 1);
            CHECK(ctx.phase == statement_phase::metadata_published);
            CHECK(ctx.completed_rows == 0 && !ctx.input_path && !ctx.input_line);
            CHECK(store.load_database(first).tables.empty());
            CHECK(store.load_root().databases[0].name == "school");
            CHECK(store.load_root().next_database_id == 2);
            CHECK(fs::is_directory(tmp.path / "databases/1/tables"));
            CHECK(fs::is_directory(tmp.path / "databases/1/indexes"));

            expect_error(error_code::object_exists,[&]{manager.create_database("school",ctx);});
            CHECK(ctx.phase == statement_phase::preparing);
            expect_error(error_code::invalid_argument,[&]{manager.create_database("",ctx);});
            expect_error(error_code::invalid_argument,[&]{manager.create_database(std::string("a\0b",3),ctx);});
            CHECK(store.load_root().next_database_id == 2);
            database_id second = manager.create_database("library",ctx);
            CHECK(second == 2);

            // Index zero is a valid lookup result; selecting an empty database succeeds.
            manager.use_database(first);
            manager.use_database(first);
            manager.use_database(second);
            expect_error(error_code::object_not_found,[&]{manager.use_database(invalid_database_id);});
            manager.use_database(first);

            fs::path second_catalog = tmp.path / "databases/2/catalog.json";
            std::string before = read_text(second_catalog);
            write_text(second_catalog,"{");
            expect_exception<json::parse_error>([&]{manager.use_database(second);});
            manager.use_database(first);
            write_text(second_catalog,before);

            write_text(tmp.path / "databases/1/tables/payload.heap","data");
            manager.drop_database(first,ctx);
            CHECK(ctx.phase == statement_phase::metadata_published);
            CHECK(!fs::exists(tmp.path / "databases/1"));
            CHECK(store.load_database(second).id == second);
            CHECK(store.load_root().databases.size() == 1);
            CHECK(store.load_root().next_database_id == 3);
            expect_error(error_code::object_not_found,[&]{manager.drop_database(first,ctx);});
            CHECK(ctx.phase == statement_phase::preparing);
            expect_error(error_code::object_not_found,[&]{manager.use_database(first);});
            manager.use_database(second);
        }
        // Reopening uses persisted registration and does not reuse deleted IDs.
        database_manager reopened(store,fm,bpm);
        CHECK(reopened.create_database("school",ctx) == 3);
        CHECK(store.load_root().databases.size() == 2);
        CHECK(store.load_root().next_database_id == 4);
    }

    void test_database_manager_failures()
    {
        temp_dir tmp;
        catalog_store store(tmp.path);
        store.init_meta();
        file_manager fm;
        buffer_pool_manager bpm(fm,3);
        database_manager manager(store,fm,bpm);
        statement_context ctx;
        fs::path directory = tmp.path / "databases/1";
        std::string before = read_text(tmp.path / "engine.meta.json");

        // An existing directory belongs to somebody else, even if unregistered.
        fs::create_directory(directory);
        write_text(directory / "sentinel","keep");
        expect_error(error_code::object_exists,[&]{manager.create_database("school",ctx);});
        CHECK(read_text(directory / "sentinel") == "keep");
        CHECK(read_text(tmp.path / "engine.meta.json") == before);
        fs::remove_all(directory);

#ifdef IDBS_CATALOG_FAILURE_TESTS
        for(bool* failure : {&fail_write,&short_write,&fail_close,&fail_rename})
        {
            *failure = true;
            expect_error(error_code::io_error,[&]{manager.create_database("school",ctx);});
            CHECK(!*failure);
            CHECK(ctx.phase == statement_phase::preparing);
            CHECK(!fs::exists(directory));
            CHECK(read_text(tmp.path / "engine.meta.json") == before);
        }
        // Database catalog succeeds, but root registration fails.
        fail_rename_after = 1;
        expect_error(error_code::io_error,[&]{manager.create_database("school",ctx);});
        CHECK(fail_rename_after == -1);
        CHECK(ctx.phase == statement_phase::preparing);
        CHECK(!fs::exists(directory));
        CHECK(read_text(tmp.path / "engine.meta.json") == before);
#endif
        CHECK(manager.create_database("school",ctx) == 1);
        write_text(directory / "tables/sentinel","keep");
        before = read_text(tmp.path / "engine.meta.json");
#ifdef IDBS_CATALOG_FAILURE_TESTS
        for(bool* failure : {&fail_write,&short_write,&fail_close,&fail_rename})
        {
            *failure = true;
            expect_error(error_code::io_error,[&]{manager.drop_database(1,ctx);});
            CHECK(!*failure);
            CHECK(ctx.phase == statement_phase::preparing);
            CHECK(read_text(directory / "tables/sentinel") == "keep");
            CHECK(read_text(tmp.path / "engine.meta.json") == before);
            CHECK(store.load_database(1).id == 1);
        }
#endif
        manager.drop_database(1,ctx);
        CHECK(ctx.phase == statement_phase::metadata_published);
        CHECK(store.load_root().databases.empty());
        CHECK(!fs::exists(directory));
    }

    void test_database_id_exhaustion()
    {
        temp_dir tmp;
        catalog_store store(tmp.path);
        store.init_meta();
        root_catalog root = store.load_root();
        root.next_database_id = invalid_database_id - 1;
        store.save_root(root);
        file_manager fm;
        buffer_pool_manager bpm(fm,3);
        database_manager manager(store,fm,bpm);
        statement_context ctx;
        database_id last = manager.create_database("last",ctx);
        CHECK(last == invalid_database_id - 1);
        CHECK(store.load_database(last).id == last);
        CHECK(store.load_root().next_database_id == invalid_database_id);
        expect_error(error_code::resource_limit,[&]{manager.create_database("school",ctx);});
        CHECK(ctx.phase == statement_phase::preparing);
        CHECK(store.load_root().databases.size() == 1);
        CHECK(store.load_root().next_database_id == invalid_database_id);
        CHECK(!fs::exists(tmp.path / "databases" / std::to_string(invalid_database_id)));

        database_manager reopened(store,fm,bpm);
        expect_error(error_code::resource_limit,[&]{reopened.create_database("school",ctx);});
        database_catalog unassigned;
        expect_error(error_code::invalid_format,[&]{store.save_database(unassigned);});
    }

    void test_definition_conversion()
    {
        database_catalog database;
        database.id = 1;
        create_table_request parent_request;
        parent_request.name = "parent";
        parent_request.columns = {{"a",data_type::INT},{"b",data_type::INT}};
        parent_request.primary_key = primary_key_definition{{"b","a"},std::nullopt};
        table_schema parent = from_definition(parent_request,database);
        CHECK(parent.id == 1);
        CHECK(parent.primary_key->id == 2 && parent.primary_key->index == 3);
        CHECK(parent.primary_key->columns == std::vector<column_id>({2,1}));
        CHECK(parent.primary_key->name == "pk_2");
        CHECK(parent.indexes[0].state == index_state::building);
        CHECK(parent.indexes[0].file_path == fs::path("indexes/3.bpt"));
        CHECK(database.next_object_id == 4 && database.tables.empty());
        database.tables.push_back(parent);

        create_table_request child_request;
        child_request.name = "child";
        child_request.columns = {{"x",data_type::INT},{"y",data_type::INT}};
        child_request.primary_key = primary_key_definition{{"x"},std::string("child_pk")};
        child_request.foreign_keys = {
            {{"x","y"},"parent",{"b","a"},std::nullopt},
            {{"x","y"},"parent",{"b","a"},std::string("second_fk")}
        };
        database_catalog converted = database;
        table_schema child = from_definition(child_request,converted);
        CHECK(child.id == 4 && child.heap_path == fs::path("tables/4.heap"));
        CHECK(child.columns[0].name == "x" && child.columns[0].id == 1 && child.columns[0].ordinal == 0);
        CHECK(child.primary_key->id == 5 && child.primary_key->index == 6);
        CHECK(child.primary_key->name == "child_pk");
        CHECK(child.foreign_keys.size() == 2 && child.indexes.size() == 3);
        CHECK(child.foreign_keys[0].id == 7 && child.foreign_keys[0].child_index == 8);
        CHECK(child.foreign_keys[1].id == 9 && child.foreign_keys[1].child_index == 10);
        CHECK(child.foreign_keys[0].name == "fk_7" && child.foreign_keys[1].name == "second_fk");
        CHECK(child.foreign_keys[0].parent_table == parent.id);
        CHECK(child.foreign_keys[0].child_columns == std::vector<column_id>({1,2}));
        CHECK(child.foreign_keys[0].parent_columns == std::vector<column_id>({2,1}));
        CHECK(child.indexes[1].origin == index_origin::foreign_key);
        CHECK(child.indexes[1].columns == child.foreign_keys[0].child_columns);
        CHECK(converted.next_object_id == 11 && converted.tables.size() == 1);

        create_table_request invalid = child_request;
        invalid.foreign_keys[0].parent_table = "missing";
        expect_error(error_code::object_not_found,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.foreign_keys[0].child_columns[0] = "missing";
        expect_error(error_code::object_not_found,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.foreign_keys[0].parent_columns = {"a","b"};
        expect_error(error_code::foreign_key_violation,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.foreign_keys[0].child_columns = {"x"};
        expect_error(error_code::foreign_key_violation,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.foreign_keys[0].child_columns = {"x","x"};
        expect_error(error_code::invalid_argument,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.columns[0].type = data_type::FLOAT;
        expect_error(error_code::foreign_key_violation,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.foreign_keys[0].name = "child_pk";
        expect_error(error_code::object_exists,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.columns[1].name = "x";
        expect_error(error_code::object_exists,[&]{from_definition(invalid,database);});
        invalid = child_request;
        invalid.columns[0].has_default = true;
        // The declaration allows NULL, but the primary key makes it effectively NOT NULL.
        expect_error(error_code::not_null_violation,[&]{from_definition(invalid,database);});
        CHECK(database.next_object_id == 4 && database.tables.size() == 1);

        create_table_request self;
        self.name = "employee";
        self.columns = {{"id",data_type::INT},{"manager_id",data_type::INT}};
        self.primary_key = primary_key_definition{{"id"},std::nullopt};
        self.foreign_keys = {{{"manager_id"},"employee",{"id"},std::nullopt}};
        table_schema employee = from_definition(self,database);
        CHECK(employee.foreign_keys[0].parent_table == employee.id);
        CHECK(employee.foreign_keys[0].child_columns == std::vector<column_id>({2}));
        CHECK(employee.foreign_keys[0].parent_columns == std::vector<column_id>({1}));

        database.next_object_id = invalid_table_id - 1;
        expect_error(error_code::resource_limit,[&]{from_definition(self,database);});
        CHECK(database.next_object_id == invalid_table_id - 1);
    }

    void test_create_table()
    {
        temp_dir tmp;
        catalog_store store(tmp.path);
        store.init_meta();
        file_manager fm;
        buffer_pool_manager bpm(fm,3);
        database_manager manager(store,fm,bpm);
        statement_context ctx;
        create_table_request request;
        request.name = "student";
        request.columns = {
            {"id",data_type::INT,0,false,false,std::monostate{}},
            {"name",data_type::VARCHAR,16,true,true,std::string("Alice")}
        };
        expect_error(error_code::no_database,[&]{manager.create_table(request,ctx);});
        database_id id = manager.create_database("school",ctx);
        manager.use_database(id);
        CHECK(manager.create_table(request,ctx) == 1);
        CHECK(ctx.phase == statement_phase::metadata_published);
        database_catalog database = store.load_database(id);
        CHECK(database.tables.size() == 1 && database.next_object_id == 2);
        CHECK(database.tables[0].columns[0].name == "id");
        CHECK(database.tables[0].columns[1].ordinal == 1);
        CHECK(std::get<std::string>(database.tables[0].columns[1].default_value) == "Alice");

        fs::path heap = tmp.path / "databases/1/tables/1.heap";
        row_codec codec({{data_type::INT,0,false},{data_type::VARCHAR,16,true}});
        const row values{std::int32_t{42},std::string("Alice")};
        rid record;
        {
            file_id fid = fm.open_file(heap.string());
            record_file records(fm,bpm,fid);
            CHECK(records.record_count() == 0 && records.record_size() == codec.record_size());
            record = records.insert(codec.encode(values));
            bpm.close_file(fid);
        }
        {
            file_id fid = fm.open_file(heap.string());
            record_file records(fm,bpm,fid);
            CHECK(codec.decode(records.get(record)) == values);
            bpm.close_file(fid);
        }
        expect_error(error_code::object_exists,[&]{manager.create_table(request,ctx);});
        request.name = "other";
        request.primary_key = primary_key_definition{{"id"},std::nullopt};
        expect_error(error_code::feature_unsupported,[&]{manager.create_table(request,ctx);});
        request.primary_key.reset();
        request.foreign_keys = {{{"id"},"student",{"id"},std::nullopt}};
        expect_error(error_code::feature_unsupported,[&]{manager.create_table(request,ctx);});
        request.foreign_keys.clear();

        fs::path next_heap = tmp.path / "databases/1/tables/2.heap";
        write_text(next_heap,"keep");
        expect_error(error_code::object_exists,[&]{manager.create_table(request,ctx);});
        CHECK(read_text(next_heap) == "keep");
        fs::remove(next_heap);
#ifdef IDBS_CATALOG_FAILURE_TESTS
        std::string before = read_text(tmp.path / "databases/1/catalog.json");
        for(bool* failure : {&fail_write,&short_write,&fail_close,&fail_rename})
        {
            *failure = true;
            expect_error(error_code::io_error,[&]{manager.create_table(request,ctx);});
            CHECK(!*failure && ctx.phase == statement_phase::preparing);
            CHECK(!fs::exists(next_heap));
            CHECK(read_text(tmp.path / "databases/1/catalog.json") == before);
        }
#endif
        CHECK(manager.create_table(request,ctx) == 2);
        CHECK(store.load_database(id).tables.size() == 2);
    }

    struct manager_fixture
    {
        temp_dir tmp;
        catalog_store store;
        file_manager fm;
        buffer_pool_manager bpm;
        std::unique_ptr<database_manager> manager;
        statement_context ctx;
        database_id school;
        database_id library;
        table_id student;
        create_table_request request;

        manager_fixture():store(tmp.path),bpm(fm,4)
        {
            store.init_meta();
            manager = std::make_unique<database_manager>(store,fm,bpm);
            school = manager->create_database("school",ctx);
            library = manager->create_database("library",ctx);
            manager->use_database(school);
            request.name = "student";
            request.columns = {{"id",data_type::INT,0,false,false,std::monostate{}},
                               {"name",data_type::VARCHAR,16,true,false,std::monostate{}}};
            student = manager->create_table(request,ctx);
        }
    };

    void test_table_leases_and_switching()
    {
        manager_fixture f;
        CHECK(f.manager->list_databases()[0].id == f.library);
        CHECK(f.manager->current_database() == f.school);
        CHECK(f.manager->list_tables()[0].id == f.student);
        table_schema copy = f.manager->describe_table(f.student);
        copy.name = "modified copy";
        CHECK(f.manager->describe_table(f.student).name == "student");
        expect_error(error_code::object_not_found,[&]{f.manager->describe_table(invalid_table_id);});
        expect_error(error_code::object_not_found,[&]{f.manager->open_table(invalid_table_id);});

        table_lease first = f.manager->open_table(f.student);
        table_lease second = f.manager->open_table(f.student);
        CHECK(first.valid() && second.valid());
        CHECK(&first.records() == &second.records());
        CHECK(&first.codec() == &second.codec());
        CHECK(&first.schema() == &second.schema());
        const row values{std::int32_t{42},std::string("Alice")};
        rid record = first.records().insert(first.codec().encode(values));
        CHECK(second.codec().decode(second.records().get(record)) == values);
        {
            record_scan scan = first.records().scan();
            std::optional<scan_item> item = scan.next();
            CHECK(item && item->id == record && first.codec().decode(item->bytes) == values);
            CHECK(!scan.next());
        }
        // Creating more tables replaces the catalog vector and rehashes the open registry.
        // Existing lease references must stay valid through both changes.
        const table_schema* stable_schema = &first.schema();
        record_file* stable_records = &first.records();
        for(int i = 0;i < 12;++i)
        {
            f.request.name = "extra_" + std::to_string(i);
            table_id id = f.manager->create_table(f.request,f.ctx);
            table_lease extra = f.manager->open_table(id);
            CHECK(extra.schema().id == id);
        }
        CHECK(&first.schema() == stable_schema && &first.records() == stable_records);
        CHECK(f.manager->list_tables()[0].name == "extra_0");
        f.manager->use_database(f.school);
        expect_error(error_code::resource_busy,[&]{f.manager->use_database(f.library);});
        expect_error(error_code::resource_busy,[&]{f.manager->drop_table(f.student,f.ctx);});
        expect_error(error_code::resource_busy,[&]{f.manager->drop_database(f.school,f.ctx);});
        expect_error(error_code::resource_busy,[&]{f.manager->close();});
        CHECK(f.manager->current_database() == f.school);

        table_lease moved(std::move(first));
        CHECK(!first.valid() && moved.valid());
        expect_error(error_code::invalid_argument,[&]{first.records();});
        expect_error(error_code::invalid_argument,[&]{first.codec();});
        expect_error(error_code::invalid_argument,[&]{first.schema();});
        second = std::move(moved);
        CHECK(!moved.valid() && second.valid());
        table_lease& same = second;
        second = std::move(same);
        CHECK(second.valid());
        expect_error(error_code::resource_busy,[&]{f.manager->close();});
        second.release();
        second.release();
        CHECK(!second.valid());
        f.manager->close();
        CHECK(!f.manager->current_database());
        f.manager->close();
        expect_error(error_code::no_database,[&]{f.manager->list_tables();});
        expect_error(error_code::no_database,[&]{f.manager->describe_table(f.student);});
        expect_error(error_code::no_database,[&]{f.manager->open_table(f.student);});
        expect_error(error_code::no_database,[&]{f.manager->drop_table(f.student,f.ctx);});

        // Rebuild all managers; the heap must be readable after explicit close.
        {
            file_manager fm;
            buffer_pool_manager bpm(fm,3);
            catalog_store store(f.tmp.path);
            database_manager reopened(store,fm,bpm);
            reopened.use_database(f.school);
            table_lease table = reopened.open_table(f.student);
            CHECK(table.codec().decode(table.records().get(record)) == values);
            table.release();
            reopened.close();
        }
        f.manager->use_database(f.school);
        {
            table_lease table = f.manager->open_table(f.student);
            table.records().update(record,table.codec().encode({std::int32_t{43},std::string("Bob")}));
        }
        f.manager->use_database(f.library);
        CHECK(f.manager->current_database() == f.library && f.manager->list_tables().empty());
        f.manager->use_database(f.school);
        {
            table_lease table = f.manager->open_table(f.student);
            CHECK(std::get<std::int32_t>(table.codec().decode(table.records().get(record))[0]) == 43);
        }
        f.manager->drop_table(f.student,f.ctx);
        CHECK(f.ctx.phase == statement_phase::metadata_published);
        CHECK(!fs::exists(f.tmp.path / "databases/1/tables/1.heap"));
        expect_error(error_code::object_not_found,[&]{f.manager->open_table(f.student);});
        f.manager->drop_database(f.school,f.ctx);
        CHECK(!f.manager->current_database());
    }

    void test_open_table_validation()
    {
        manager_fixture f;
        fs::path heap = f.tmp.path / "databases/1/tables/1.heap";
        std::string before = read_text(heap);
        write_text(heap,"bad header");
        expect_error(error_code::invalid_format,[&]{f.manager->open_table(f.student);});
        write_text(heap,before);
        {
            table_lease table = f.manager->open_table(f.student);
            CHECK(table.records().record_count() == 0);
        }
        f.manager->close();
        database_catalog database = f.store.load_database(f.school);
        database.tables[0].columns[1].max_length = 32;
        f.store.save_database(database);
        f.manager->use_database(f.school);
        expect_error(error_code::invalid_format,[&]{f.manager->open_table(f.student);});
        f.manager->close();
        database.tables[0].columns[1].max_length = 16;
        f.store.save_database(database);
        f.manager->use_database(f.school);
        {
            table_lease table = f.manager->open_table(f.student);
            CHECK(table.records().record_size() == table.codec().record_size());
        }
        // Destruction also closes released tables, even without an explicit close().
        f.manager.reset();
        file_id fid = f.fm.open_file(heap.string());
        f.bpm.close_file(fid);
    }

    void test_drop_dependencies()
    {
        fixture f;
        file_manager fm;
        buffer_pool_manager bpm(fm,3);
        database_manager manager(f.store,fm,bpm);
        statement_context ctx;
        manager.use_database(1);
        expect_error(error_code::dependency_exists,[&]{manager.drop_table(1,ctx);});
        CHECK(ctx.phase == statement_phase::preparing && manager.list_tables().size() == 2);
        manager.drop_table(2,ctx);
        CHECK(!fs::exists(f.database_file.parent_path() / "tables/2.heap"));
        for(index_id id : {5,7,8})
            CHECK(!fs::exists(f.database_file.parent_path() / "indexes" / (std::to_string(id) + ".bpt")));
        manager.drop_table(1,ctx);
        CHECK(manager.list_tables().empty());
        CHECK(fs::is_empty(f.database_file.parent_path() / "tables"));
        CHECK(fs::is_empty(f.database_file.parent_path() / "indexes"));
    }

    void test_table_lifecycle_failures()
    {
#ifdef IDBS_CATALOG_FAILURE_TESTS
        manager_fixture f;
        const row values{std::int32_t{7},std::string("saved")};
        rid record;
        {
            table_lease table = f.manager->open_table(f.student);
            record = table.records().insert(table.codec().encode(values));
        }
        fail_write = true;
        expect_error(error_code::io_error,[&]{f.manager->use_database(f.library);});
        CHECK(!fail_write && f.manager->current_database() == f.school);
        {
            table_lease table = f.manager->open_table(f.student);
            CHECK(table.codec().decode(table.records().get(record)) == values);
        }
        fail_close = true;
        expect_error(error_code::io_error,[&]{f.manager->close();});
        CHECK(!fail_close && f.manager->current_database() == f.school);
        {
            table_lease table = f.manager->open_table(f.student);
            CHECK(table.codec().decode(table.records().get(record)) == values);
        }
        fs::path catalog = f.tmp.path / "databases/1/catalog.json";
        fs::path heap = f.tmp.path / "databases/1/tables/1.heap";
        std::string before = read_text(catalog);
        fail_rename = true;
        expect_error(error_code::io_error,[&]{f.manager->drop_table(f.student,f.ctx);});
        CHECK(!fail_rename && f.ctx.phase == statement_phase::preparing);
        CHECK(read_text(catalog) == before && fs::exists(heap));
        CHECK(f.manager->describe_table(f.student).id == f.student);
        {
            table_lease table = f.manager->open_table(f.student);
            CHECK(table.codec().decode(table.records().get(record)) == values);
        }
        fail_unlink = true;
        expect_error(error_code::io_error,[&]{f.manager->drop_table(f.student,f.ctx);});
        CHECK(!fail_unlink && f.ctx.phase == statement_phase::metadata_published);
        CHECK(f.manager->list_tables().empty());
        CHECK(f.store.load_database(f.school).tables.empty());
        CHECK(fs::exists(heap));
        expect_error(error_code::object_not_found,[&]{f.manager->open_table(f.student);});
#endif
    }

    void test_paths_and_failed_saves()
    {
        fixture f;
        std::string before = read_text(f.database_file);
        auto invalid = f.database;
        invalid.tables[0].columns[2].default_value = std::numeric_limits<double>::infinity();
        expect_error(error_code::invalid_format,[&]{f.store.save_database(invalid);});
        CHECK(read_text(f.database_file) == before);
        invalid = f.database;
        invalid.tables[0].indexes[0].origin = static_cast<index_origin>(99);
        expect_error(error_code::invalid_format,[&]{f.store.save_database(invalid);});
        CHECK(read_text(f.database_file) == before);

        fs::path directory = f.database_file.parent_path();
        write_text(f.tmp.path / "outside.heap","sentinel");
        fs::create_symlink(f.tmp.path / "outside.heap",directory / "tables" / "escape.heap");
        corrupt(f,false,[](json& j){j["tables"][0]["heap_path"] = "tables/escape.heap";});
        CHECK(read_text(f.tmp.path / "outside.heap") == "sentinel");

        database_catalog missing;
        missing.id = 99;
        expect_error(error_code::io_error,[&]{f.store.save_database(missing);});
        CHECK(!fs::exists(f.store.data_root() / "databases" / "99"));
#ifdef IDBS_CATALOG_FAILURE_TESTS
        auto root = f.store.load_root();
        root.databases[0].name = "renamed";
        std::string root_before = read_text(f.root_file);
        for(bool* failure : {&fail_write,&short_write,&fail_close,&fail_rename})
        {
            *failure = true;
            expect_error(error_code::io_error,[&]{f.store.save_root(root);});
            CHECK(!*failure);
            CHECK(read_text(f.root_file) == root_before);
            CHECK(f.store.load_root().databases[0].name == "school");
            for(const auto& entry : fs::directory_iterator(f.store.data_root()))
                CHECK(entry.path().filename().string().find(".tmp.") == std::string::npos);
        }
        fail_rename = true;
        expect_error(error_code::io_error,[&]{f.store.save_database(f.database);});
        CHECK(read_text(f.database_file) == before);
        f.store.save_root(root);
        CHECK(f.store.load_root().databases[0].name == "renamed");
#endif
    }
}

int main()
{
    try
    {
        test_initialization();
        test_round_trip();
        test_record_reopen();
        test_corrupt_catalogs();
        test_paths_and_failed_saves();
        test_database_manager();
        test_database_manager_failures();
        test_database_id_exhaustion();
        test_definition_conversion();
        test_create_table();
        test_table_leases_and_switching();
        test_open_table_validation();
        test_drop_dependencies();
        test_table_lifecycle_failures();
    }
    catch(const std::exception& error)
    {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return failures == 0?0:1;
}
