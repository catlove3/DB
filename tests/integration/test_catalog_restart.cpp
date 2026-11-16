#include <db/catalog/catalog.h>
#include <db/storage/buffer_pool.h>
#include <iostream>
#include <stdexcept>

using namespace db;
namespace fs = std::filesystem;

namespace
{
    constexpr std::int32_t row_count = 1000;

    void check(bool condition,const char* message)
    {
        if(!condition)
            throw std::runtime_error(message);
    }

    create_table_request student_definition()
    {
        create_table_request request;
        request.name = "student";
        request.columns = {
            {"id",data_type::INT,0,false,false,std::monostate{}},
            {"name",data_type::VARCHAR,32,false,true,std::string("unknown")},
            {"score",data_type::FLOAT,0,true,false,std::monostate{}}
        };
        return request;
    }

    row student_row(std::int32_t id,bool updated)
    {
        std::string name = (updated?"updated_":"student_") + std::to_string(id);
        value score = std::monostate{};
        if(id % 3 != 0)
            score = static_cast<double>(id) / 4;
        return {id,name,score};
    }

    void prepare(catalog_store& store,database_manager& manager)
    {
        statement_context ctx;
        database_id school = manager.create_database("school",ctx);
        manager.create_database("library",ctx);
        manager.use_database(school);
        create_table_request request = student_definition();
        table_id student = manager.create_table(request,ctx);
        request.name = "course";
        manager.create_table(request,ctx);
        table_lease table = manager.open_table(student);
        std::vector<rid> positions;
        positions.reserve(row_count);
        for(std::int32_t i = 0;i < row_count;++i)
            positions.push_back(table.records().insert(table.codec().encode(student_row(i,false))));
        for(std::int32_t i = 0;i < row_count;++i)
        {
            if(i % 5 == 0)
                table.records().erase(positions[i]);
            else if(i % 7 == 0)
                table.records().update(positions[i],table.codec().encode(student_row(i,true)));
        }
        check(table.records().record_count() == 800,"wrong record count before close");
        table.release();
        manager.close();
        check(fs::file_size(store.data_root() / "databases/1/tables/1.heap") > 5 * page_size,
              "test must use more data pages than the three-frame buffer");
    }

    void verify_and_replace(catalog_store& store,database_manager& manager)
    {
        check(!manager.current_database(),"selection must not persist between processes");
        std::vector<database_info> databases = manager.list_databases();
        check(databases.size() == 2 && databases[0].name == "library" && databases[1].name == "school",
              "database names did not survive restart");
        manager.use_database(databases[1].id);
        std::vector<table_info> tables = manager.list_tables();
        check(tables.size() == 2 && tables[0].name == "course" && tables[1].name == "student",
              "table names did not survive restart");
        table_lease table = manager.open_table(tables[1].id);
        check(table.schema().columns[1].has_default &&
              std::get<std::string>(table.schema().columns[1].default_value) == "unknown",
              "column default did not survive restart");
        check(table.records().record_count() == 800,"record count did not survive restart");
        {
            record_scan scan = table.records().scan();
            for(std::int32_t i = 0;i < row_count;++i)
            {
                if(i % 5 == 0)
                    continue;
                std::optional<scan_item> item = scan.next();
                check(item.has_value(),"missing record after restart");
                check(table.codec().decode(item->bytes) == student_row(i,i % 7 == 0),
                      "insert/update/delete or row encoding did not survive restart");
            }
            check(!scan.next(),"extra record after restart");
        }
        table.release();
        {
            table_lease course = manager.open_table(tables[0].id);
            check(course.records().record_count() == 0,"empty heap did not survive restart");
        }
        statement_context ctx;
        manager.drop_table(tables[1].id,ctx);
        check(!fs::exists(store.data_root() / "databases/1/tables/1.heap"),"DROP TABLE left its heap");
        check(manager.list_tables().size() == 1,"DROP TABLE affected the other table");
        manager.drop_database(databases[1].id,ctx);
        check(!manager.current_database(),"DROP DATABASE retained current selection");
        database_id replacement = manager.create_database("school",ctx);
        check(replacement == 3,"deleted database ID was reused");
        manager.use_database(replacement);
        table_id student = manager.create_table(student_definition(),ctx);
        {
            table_lease replacement_table = manager.open_table(student);
            replacement_table.records().insert(replacement_table.codec().encode(
                {std::int32_t{42},std::string("persisted"),1.5}));
        }
        manager.close();
    }

    void verify_final(catalog_store& store,database_manager& manager)
    {
        root_catalog root = store.load_root();
        check(root.next_database_id == 4 && root.databases.size() == 2,"root allocator was not persisted");
        check(!fs::exists(store.data_root() / "databases/1"),"dropped database returned after restart");
        manager.use_database(2);
        check(manager.list_tables().empty(),"unrelated database changed");
        manager.use_database(3);
        check(manager.list_tables().size() == 1,"replacement table is missing");
        {
            table_lease table = manager.open_table(1);
            record_scan scan = table.records().scan();
            std::optional<scan_item> item = scan.next();
            check(item.has_value(),"replacement record is missing");
            check(table.codec().decode(item->bytes) == row({std::int32_t{42},std::string("persisted"),1.5}),
                  "replacement record changed");
            check(!scan.next(),"deleted records leaked into the replacement database");
        }
        manager.close();
    }
}

int main(int argc,char** argv)
{
    try
    {
        if(argc != 3)
            throw std::runtime_error("expected a phase and a data directory");
        std::string phase = argv[1];
        catalog_store store(argv[2]);
        if(phase == "prepare")
            store.init_meta();
        file_manager fm;
        buffer_pool_manager bpm(fm,3);
        database_manager manager(store,fm,bpm);
        if(phase == "prepare")
            prepare(store,manager);
        else if(phase == "verify_and_replace")
            verify_and_replace(store,manager);
        else if(phase == "verify_final")
            verify_final(store,manager);
        else
            throw std::runtime_error("unknown restart test phase");
    }
    catch(const std::exception& error)
    {
        std::cerr << "Catalog restart test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
