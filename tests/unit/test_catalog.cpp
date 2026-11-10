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
#include <unistd.h>

using namespace db;
using json = nlohmann::json;
namespace fs = std::filesystem;

#ifdef IDBS_CATALOG_FAILURE_TESTS
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_close(int);
extern "C" int __real_rename(const char*,const char*);
namespace
{
    bool fail_write = false;
    bool fail_close = false;
    bool fail_rename = false;
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
    if(fail_rename)
    {
        fail_rename = false;
        errno = EIO;
        return -1;
    }
    return __real_rename(from,to);
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
        expect_error(error_code::io_error,[&]{blocked.init_meta();});
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

    template<class F>
    void corrupt(fixture& f,bool root,F&& change)
    {
        const fs::path& file = root?f.root_file:f.database_file;
        std::string before = read_text(file);
        json j = json::parse(before);
        change(j);
        write_text(file,j.dump());
        expect_error(error_code::invalid_format,[&]
        {
            if(root)
                f.store.load_root();
            else
                f.store.load_database(1);
        });
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
        corrupt(f,true,[](json& j){j.erase("next_database_id");});
        corrupt(f,false,[](json& j){j["id"] = 2;});
        corrupt(f,false,[](json& j){j["next_object_id"] = 8;});
        corrupt(f,false,[](json& j){j["tables"] = json::object();});
        corrupt(f,false,[](json& j){j["tables"][0]["record_format_version"] = 99;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["ordinal"] = -1;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["id"] = 4294967297ULL;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["id"] = 1;});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["type"] = "UNKNOWN";});
        corrupt(f,false,[](json& j){j["tables"][0]["columns"][1]["declared_nullable"] = 1;});
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
        corrupt(f,false,[](json& j){j["tables"][0]["indexes"][0]["origin"] = 2;});
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
        for(const std::string& text : {std::string("{"),std::string("null"),before + " trailing"})
        {
            write_text(f.database_file,text);
            expect_error(error_code::invalid_format,[&]{f.store.load_database(1);});
        }
        write_text(f.database_file,before);
        fs::remove(f.database_file);
        expect_error(error_code::invalid_format,[&]{f.store.load_database(1);});
        expect_error(error_code::object_not_found,[&]{f.store.load_database(999);});
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
    }
    catch(const std::exception& error)
    {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
    return failures == 0?0:1;
}
