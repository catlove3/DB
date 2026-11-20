#include <db/parser/parser.h>
#include <db/common/error.h>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using namespace db;

#define CHECK(condition) \
    do { \
        if(!(condition)) \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #condition); \
    } while(false)

template<class T, class Base>
const T& as(const node_ptr<Base>& node)
{
    CHECK(node != nullptr);
    const auto* typed = dynamic_cast<const T*>(node.get());
    CHECK(typed != nullptr);
    return *typed;
}

template<class T>
node_ptr<T> parse_as(std::string_view sql)
{
    auto result = parser{}.parse_one(sql);
    auto* typed = dynamic_cast<T*>(result.get());
    if(!typed)
        throw std::runtime_error("wrong AST type for SQL: " + std::string(sql));
    result.release();
    return node_ptr<T>(typed);
}

// The marker is unique in these ASCII fixtures. Compute its location independently
// of ANTLR to check the source positions retained in the owned AST.
void check_position(source_position actual, std::string_view sql, std::string_view marker)
{
    const auto offset = sql.find(marker);
    CHECK(offset != std::string_view::npos);
    source_position expected;
    for(std::size_t i = 0; i < offset; ++i)
    {
        if(sql[i] == '\n')
        {
            ++expected.line;
            expected.column = 1;
        }
        else
            ++expected.column;
    }
    CHECK(actual.line == expected.line);
    CHECK(actual.column == expected.column);
}

void simple_commands()
{
    CHECK(parse_as<create_database_statement>("CREATE DATABASE School;")->name.text == "School");
    CHECK(parse_as<drop_database_statement>("DROP DATABASE missing_db;")->name.text == "missing_db");
    auto use = parse_as<use_database_statement>("\n  USE School;");
    CHECK(use->name.text == "School");
    CHECK(use->position.line == 2 && use->position.column == 3);
    CHECK(use->name.position.line == 2 && use->name.position.column == 7);
    CHECK(parse_as<drop_table_statement>("DROP TABLE students;")->table.text == "students");
    CHECK(parse_as<describe_table_statement>("DESC students;")->table.text == "students");
    parse_as<show_databases_statement>("SHOW DATABASES;");
    parse_as<show_tables_statement>("SHOW TABLES;");
    parse_as<show_indexes_statement>("SHOW INDEXES;");
    for(const char* sql : {"NULL;", "-- comment;", "---;", "--\n;", "-- line one\nline two;"})
        parse_as<noop_statement>(sql);
    auto comment = parse_as<noop_statement>("\n -- comment;");
    CHECK(comment->position.line == 2 && comment->position.column == 2);
}

void insert_and_load()
{
    std::string sql = "\n INSERT INTO missing_table VALUES\n (0001, -2.5, 'a;汉', NULL),\n (3, 4., '', 5);";
    auto insert = parse_as<insert_statement>(sql);
    sql.assign(20000, 'x'); // Neither input nor parser objects own the AST's text.
    CHECK(insert->table.text == "missing_table");
    CHECK(insert->position.line == 2 && insert->position.column == 2);
    CHECK(insert->rows.size() == 2);
    CHECK(insert->rows[0].size() == 4 && insert->rows[1].size() == 4);
    CHECK(as<integer_literal>(insert->rows[0][0]).text == "0001");
    CHECK(insert->rows[0][0]->position.line == 3 && insert->rows[0][0]->position.column == 3);
    CHECK(as<floating_literal>(insert->rows[0][1]).text == "-2.5");
    CHECK(as<string_literal>(insert->rows[0][2]).text == "'a;汉'");
    CHECK(as<null_literal>(insert->rows[0][3]).text == "NULL");
    CHECK(as<integer_literal>(insert->rows[1][0]).text == "3");
    CHECK(as<floating_literal>(insert->rows[1][1]).text == "4.");
    CHECK(as<string_literal>(insert->rows[1][2]).text == "''");
    // Row arity and numeric ranges are binding concerns, not syntax errors.
    auto uneven = parse_as<insert_statement>("INSERT INTO t VALUES (1), (2, 999999999999999999999999);");
    CHECK(uneven->rows[0].size() == 1 && uneven->rows[1].size() == 2);
    CHECK(uneven->rows[1][1]->text == "999999999999999999999999");

    const std::string load_sql =
        "\nLOAD DATA INFILE '/no/such/file;data.csv' INTO TABLE t\nFIELDS TERMINATED BY ',';";
    auto load = parse_as<load_statement>(load_sql);
    CHECK(load->table.text == "t");
    CHECK(load->path.text == "'/no/such/file;data.csv'");
    CHECK(load->delimiter.text == "','");
    check_position(load->path.position, load_sql, "'/no/such");
    check_position(load->delimiter.position, load_sql, "','");
    check_position(load->position, load_sql, "LOAD");
    auto empty = parse_as<load_statement>("LOAD DATA INFILE '' INTO TABLE t FIELDS TERMINATED BY '';" );
    CHECK(empty->path.text == "''" && empty->delimiter.text == "''");
}

void update_and_delete()
{
    const std::string sql =
        "UPDATE t SET id = 42, score = -1.5, name = 'DEFAULT NULL', value = NULL\n"
        "WHERE id IN (SELECT student_id FROM scores WHERE score > 90) AND name LIKE 'a%';";
    auto update = parse_as<update_statement>(sql);
    CHECK(update->table.text == "t");
    CHECK(update->assignments.size() == 4);
    CHECK(update->assignments[0].column.text == "id");
    CHECK(as<integer_literal>(update->assignments[0].replacement).text == "42");
    CHECK(update->assignments[1].column.text == "score");
    CHECK(as<floating_literal>(update->assignments[1].replacement).text == "-1.5");
    CHECK(update->assignments[2].column.text == "name");
    CHECK(as<string_literal>(update->assignments[2].replacement).text == "'DEFAULT NULL'");
    CHECK(update->assignments[3].column.text == "value");
    as<null_literal>(update->assignments[3].replacement);
    check_position(update->assignments[1].column.position, sql, "score =");
    check_position(update->assignments[1].replacement->position, sql, "-1.5");
    CHECK(update->conditions.size() == 2);
    const auto& subquery = as<subquery_expression>(as<in_predicate>(update->conditions[0]).right);
    CHECK(subquery.query->tables[0].text == "scores");
    CHECK(as<like_predicate>(update->conditions[1]).pattern.text == "'a%'");
    auto repeated = parse_as<update_statement>("UPDATE t SET a = 1, a = 2 WHERE a = 3;");
    CHECK(repeated->assignments.size() == 2); // Do not collapse duplicate assignments.

    auto all = parse_as<delete_statement>("DELETE FROM t;");
    CHECK(all->table.text == "t" && all->conditions.empty());
    auto filtered = parse_as<delete_statement>(
        "DELETE FROM t WHERE a IS NOT NULL AND b = (SELECT MAX(b) FROM u);");
    CHECK(filtered->conditions.size() == 2);
    as<is_not_null_predicate>(filtered->conditions[0]);
    const auto& inner = as<subquery_expression>(as<equal_predicate>(filtered->conditions[1]).right);
    as<maximum_expression>(inner.query->selectors[0]);
}

void column_definitions()
{
    const std::string sql =
        "CREATE TABLE Students (\n"
        " id INT NOT NULL DEFAULT 0001,\n"
        " name VARCHAR(032) DEFAULT 'NOT NULL',\n"
        " score FLOAT DEFAULT -2.5,\n"
        " optional INT DEFAULT NULL,\n"
        " plain INT,\n"
        " required INT NOT NULL,\n"
        " conflict INT NOT NULL DEFAULT NULL\n);";
    auto table = parse_as<create_table_statement>(sql);
    CHECK(table->table.text == "Students");
    CHECK(table->fields.size() == 7);
    const auto& id = as<column_declaration>(table->fields[0]);
    CHECK(id.name.text == "id");
    as<integer_type>(id.type);
    CHECK(id.not_null && id.default_value);
    CHECK(as<integer_literal>(id.default_value->value).text == "0001");
    check_position(id.position, sql, "id INT");
    check_position(id.name.position, sql, "id INT");
    check_position(id.type->position, sql, "INT");
    check_position(id.not_null->position, sql, "NOT NULL");
    check_position(id.default_value->position, sql, "DEFAULT");
    check_position(id.default_value->value->position, sql, "0001");
    const auto& name = as<column_declaration>(table->fields[1]);
    const auto& varchar = as<varchar_type>(name.type);
    CHECK(varchar.max_length.text == "032");
    check_position(varchar.max_length.position, sql, "032");
    CHECK(!name.not_null && name.default_value);
    CHECK(as<string_literal>(name.default_value->value).text == "'NOT NULL'");
    const auto& score = as<column_declaration>(table->fields[2]);
    as<floating_type>(score.type);
    CHECK(as<floating_literal>(score.default_value->value).text == "-2.5");
    const auto& nullable = as<column_declaration>(table->fields[3]);
    CHECK(!nullable.not_null && nullable.default_value);
    as<null_literal>(nullable.default_value->value);
    const auto& plain = as<column_declaration>(table->fields[4]);
    CHECK(!plain.not_null && !plain.default_value);
    const auto& required = as<column_declaration>(table->fields[5]);
    CHECK(required.not_null && !required.default_value);
    const auto& conflict = as<column_declaration>(table->fields[6]);
    CHECK(conflict.not_null && conflict.default_value);
    as<null_literal>(conflict.default_value->value);

    auto lengths = parse_as<create_table_statement>(
        "CREATE TABLE t (a VARCHAR(0), b VARCHAR(999999999999999999999));");
    CHECK(as<varchar_type>(as<column_declaration>(lengths->fields[0]).type).max_length.text == "0");
    CHECK(as<varchar_type>(as<column_declaration>(lengths->fields[1]).type).max_length.text == "999999999999999999999");
}

void create_keys()
{
    const std::string sql =
        "CREATE TABLE child (a INT,\n PRIMARY KEY pk (a, b), b INT,\n"
        " FOREIGN KEY fk (b, a) REFERENCES parent (y, x));";
    auto table = parse_as<create_table_statement>(sql);
    CHECK(table->fields.size() == 4);
    as<column_declaration>(table->fields[0]);
    const auto& pk = as<primary_key_declaration>(table->fields[1]);
    CHECK(pk.name->text == "pk");
    CHECK(pk.columns.size() == 2 && pk.columns[0].text == "a" && pk.columns[1].text == "b");
    check_position(pk.position, sql, "PRIMARY");
    check_position(pk.name->position, sql, "pk");
    as<column_declaration>(table->fields[2]);
    const auto& fk = as<foreign_key_declaration>(table->fields[3]);
    CHECK(fk.name->text == "fk" && fk.parent_table.text == "parent");
    CHECK(fk.columns.size() == 2 && fk.columns[0].text == "b" && fk.columns[1].text == "a");
    CHECK(fk.parent_columns.size() == 2 && fk.parent_columns[0].text == "y" && fk.parent_columns[1].text == "x");
    check_position(fk.position, sql, "FOREIGN");
    check_position(fk.parent_table.position, sql, "parent");
    check_position(fk.parent_columns[0].position, sql, "y");

    auto unnamed = parse_as<create_table_statement>(
        "CREATE TABLE child (a INT, PRIMARY KEY (a), FOREIGN KEY (a) REFERENCES parent (id));");
    CHECK(!as<primary_key_declaration>(unnamed->fields[1]).name);
    const auto& no_name = as<foreign_key_declaration>(unnamed->fields[2]);
    CHECK(!no_name.name && no_name.parent_table.text == "parent");
    // Grammar allows constraints before columns and mismatched FK arity.
    auto syntax_only = parse_as<create_table_statement>(
        "CREATE TABLE t (FOREIGN KEY (a, b) REFERENCES p (id), a INT, a FLOAT);");
    CHECK(as<foreign_key_declaration>(syntax_only->fields[0]).columns.size() == 2);
    CHECK(as<column_declaration>(syntax_only->fields[2]).name.text == "a");
}

void alter_add()
{
    for(bool named : {false, true})
    {
        const std::string sql = "ALTER TABLE t ADD INDEX " + std::string(named ? "idx " : "") + "(b, a);";
        auto add = parse_as<add_key_statement>(sql);
        CHECK(add->table.text == "t");
        const auto& key = as<index_declaration>(add->key);
        CHECK(key.name.has_value() == named);
        if(named) CHECK(key.name->text == "idx");
        CHECK(key.columns.size() == 2 && key.columns[0].text == "b" && key.columns[1].text == "a");
        check_position(key.position, sql, "INDEX");
        const std::string unique_sql = "ALTER TABLE t ADD UNIQUE " + std::string(named ? "uq " : "") + "(a);";
        auto unique = parse_as<add_key_statement>(unique_sql);
        const auto& uq = as<unique_declaration>(unique->key);
        CHECK(uq.name.has_value() == named && uq.columns[0].text == "a");
        if(named) CHECK(uq.name->text == "uq");
        check_position(uq.position, unique_sql, "UNIQUE");
    }

    for(const std::string prefix : {"", "CONSTRAINT ", "CONSTRAINT constraint_name "})
    {
        const bool named = prefix == "CONSTRAINT constraint_name ";
        const std::string pk_sql = "ALTER TABLE t ADD " + prefix + "PRIMARY KEY (b, a);";
        auto add_pk = parse_as<add_key_statement>(pk_sql);
        CHECK(add_pk->table.text == "t");
        const auto& pk = as<primary_key_declaration>(add_pk->key);
        CHECK(pk.name.has_value() == named);
        if(named) CHECK(pk.name->text == "constraint_name");
        CHECK(pk.columns.size() == 2 && pk.columns[0].text == "b");
        check_position(pk.position, pk_sql, "PRIMARY");
        const std::string fk_sql = "ALTER TABLE t\n ADD " + prefix +
            "FOREIGN KEY (b, a) REFERENCES parent (y, x);";
        auto add_fk = parse_as<add_key_statement>(fk_sql);
        CHECK(add_fk->table.text == "t");
        const auto& fk = as<foreign_key_declaration>(add_fk->key);
        CHECK(fk.name.has_value() == named);
        if(named)
        {
            CHECK(fk.name->text == "constraint_name");
            check_position(fk.name->position, fk_sql, "constraint_name");
        }
        CHECK(fk.parent_table.text == "parent");
        CHECK(fk.columns[0].text == "b" && fk.columns[1].text == "a");
        CHECK(fk.parent_columns[0].text == "y" && fk.parent_columns[1].text == "x");
        check_position(fk.position, fk_sql, "FOREIGN");
        check_position(fk.parent_table.position, fk_sql, "parent");
    }
}

void alter_drop()
{
    auto index = parse_as<drop_index_statement>("ALTER TABLE t DROP INDEX idx;");
    CHECK(index->table.text == "t" && index->name.text == "idx");
    CHECK(index->name.position.column == 26);
    auto foreign = parse_as<drop_foreign_key_statement>("ALTER TABLE t DROP FOREIGN KEY fk;");
    CHECK(foreign->table.text == "t" && foreign->name.text == "fk");
    auto unnamed = parse_as<drop_primary_key_statement>("ALTER TABLE t DROP PRIMARY KEY;");
    CHECK(unnamed->table.text == "t" && !unnamed->name);
    auto named = parse_as<drop_primary_key_statement>("ALTER TABLE t DROP PRIMARY KEY pk;");
    CHECK(named->name->text == "pk");
}

void bounded_input_and_ownership()
{
    node_ptr<create_table_statement> table;
    {
        const std::string sql =
            "CREATE TABLE child_table (id INT DEFAULT 0007, "
            "label VARCHAR(24) DEFAULT 'a long default; -- text', "
            "FOREIGN KEY fk_parent (id) REFERENCES parent_table (parent_id));";
        std::string storage = "ignored prefix" + sql + "DROP TABLE child_table;";
        // The view starts inside a buffer and ends before another valid statement.
        const std::string_view view(storage.data() + std::string_view("ignored prefix").size(), sql.size());
        table = parse_as<create_table_statement>(view);
        auto independent = parse_as<create_table_statement>(view);
        independent->table.text = "changed";
        CHECK(table->table.text == "child_table");
        storage.assign(storage.size(), 'x');
    }
    // Both the input storage and all ANTLR objects have been destroyed here.
    CHECK(table->table.text == "child_table");
    CHECK(table->position.line == 1 && table->position.column == 1);
    CHECK(table->fields.size() == 3);
    const auto& id = as<column_declaration>(table->fields[0]);
    CHECK(id.name.text == "id");
    CHECK(as<integer_literal>(id.default_value->value).text == "0007");
    const auto& label = as<column_declaration>(table->fields[1]);
    CHECK(as<varchar_type>(label.type).max_length.text == "24");
    CHECK(as<string_literal>(label.default_value->value).text == "'a long default; -- text'");
    const auto& foreign = as<foreign_key_declaration>(table->fields[2]);
    CHECK(foreign.name && foreign.name->text == "fk_parent");
    CHECK(foreign.columns.size() == 1 && foreign.columns[0].text == "id");
    CHECK(foreign.parent_table.text == "parent_table");
    CHECK(foreign.parent_columns.size() == 1 && foreign.parent_columns[0].text == "parent_id");
}

void multiline_literal_positions()
{
    auto table = parse_as<create_table_statement>(
        "CREATE TABLE t (\r\n label VARCHAR(20) DEFAULT '汉;\r\nexit',\r\n"
        " id INT NOT NULL DEFAULT NULL);");
    const auto& label = as<column_declaration>(table->fields[0]);
    CHECK(as<string_literal>(label.default_value->value).text == "'汉;\r\nexit'");
    CHECK(!label.not_null);
    const auto& id = as<column_declaration>(table->fields[1]);
    CHECK(id.position.line == 4 && id.position.column == 2);
    CHECK(id.type->position.line == 4 && id.type->position.column == 5);
    CHECK(id.not_null && id.default_value);
    CHECK(id.not_null->position.line == 4 && id.not_null->position.column == 9);
    CHECK(id.default_value->position.line == 4 && id.default_value->position.column == 18);
    CHECK(as<null_literal>(id.default_value->value).position.column == 26);

    // ANTLR columns count Unicode code points, not the bytes in UTF-8 text.
    auto insert = parse_as<insert_statement>("INSERT INTO t VALUES ('汉', NULL);");
    const auto& null = as<null_literal>(insert->rows[0][1]);
    CHECK(null.position.line == 1 && null.position.column == 28);
}

void positioned_statement_errors()
{
    struct error_case
    {
        std::string_view sql;
        std::size_t line;
        std::size_t column;
    };
    const error_case cases[] = {
        {"CREATE TABLE t (\n id INT,\n);", 3, 1},
        {"ALTER TABLE t ADD FOREIGN KEY (a)\nREFERENCES p ();", 2, 15},
        {"UPDATE t SET a = 1\r\nWHERE id IN (SELECT FROM u);", 2, 21},
        {"INSERT INTO t VALUES ('汉', @);", 1, 28},
        {"LOAD DATA INFILE 'x' INTO TABLE t\nFIELDS TERMINATED BY ;", 2, 22},
        {"CREATE TABLE t (a INT);\n DROP TABLE t;", 2, 2},
        // The backing literal has a semicolon, but it is outside this view.
        {std::string_view("USE db;", 6), 1, 7},
        // An embedded NUL outside a literal must not silently truncate input.
        {std::string_view("USE db;\0", 8), 1, 8}
    };
    parser facade;
    for(const auto& item : cases)
    {
        bool rejected = false;
        try { auto ast = facade.parse_one(item.sql); }
        catch(const db_error& error)
        {
            rejected = true;
            CHECK(error.code() == error_code::syntax_error);
            CHECK(error.context().operation == "parse_one");
            CHECK(error.context().line == item.line);
            CHECK(error.context().column == item.column);
        }
        CHECK(rejected);
        auto next = facade.parse_one("CREATE TABLE recovered (a INT);");
        CHECK(as<create_table_statement>(next).table.text == "recovered");
    }
}

void reader_script_and_parser_recovery()
{
    std::istringstream input(
        "-- setup\r\ncomment; USE school; CREATE TABLE t (id INT, label VARCHAR(20));\r\n"
        "ALTER TABLE t ADD PRIMARY KEY (id); INSERT INTO t VALUES (1, 'a;\r\nexit\r\n-- b');\r\n"
        "UPDATE t SET label = 'updated' WHERE id = 1; DELETE t; DELETE FROM t WHERE id = 2;\r\n"
        "SELECT label FROM t; LOAD DATA INFILE 'data;file.csv' INTO TABLE t FIELDS TERMINATED BY ',';\r\n"
        "DROP TABLE t;\r\nexit\r\nUSE must_not_be_read;");
    statement_reader reader(input);
    parser facade;
    const auto next_ast = [&]() {
        const auto sql = reader.next();
        CHECK(sql.has_value());
        return facade.parse_one(*sql);
    };
    CHECK(as<use_database_statement>(next_ast()).name.text == "school");
    CHECK(as<create_table_statement>(next_ast()).fields.size() == 2);
    auto add = next_ast();
    CHECK(as<primary_key_declaration>(as<add_key_statement>(add).key).columns[0].text == "id");
    auto insert = next_ast();
    CHECK(as<insert_statement>(insert).rows[0][1]->text == "'a;\r\nexit\r\n-- b'");
    auto update = next_ast();
    CHECK(as<update_statement>(update).assignments[0].replacement->text == "'updated'");

    // A complete but invalid SQL statement fails in the parser, not the reader.
    bool rejected = false;
    try { auto invalid = next_ast(); }
    catch(const db_error& error)
    {
        rejected = true;
        CHECK(error.code() == error_code::syntax_error);
        CHECK(error.context().operation == "parse_one");
    }
    CHECK(rejected);
    auto deletion = next_ast();
    CHECK(as<delete_statement>(deletion).conditions.size() == 1);
    auto select = next_ast();
    CHECK(as<column_expression>(as<select_statement>(select).selectors[0]).column.column.text == "label");
    auto load = next_ast();
    CHECK(as<load_statement>(load).path.text == "'data;file.csv'");
    CHECK(as<drop_table_statement>(next_ast()).table.text == "t");
    CHECK(!reader.next());
    CHECK(!reader.next());
    std::string remaining;
    CHECK(static_cast<bool>(std::getline(input, remaining)));
    CHECK(remaining == "USE must_not_be_read;");
}

void invalid_statements_and_reader()
{
    parser facade;
    for(const char* sql : {
        "USE;", "INSERT INTO t VALUES ();", "INSERT INTO t (a) VALUES (1);",
        "UPDATE t SET a = 1;", "UPDATE t SET a = 1, b = WHERE id = 1;",
        "DELETE t;", "CREATE TABLE t ();", "CREATE TABLE t (a INTEGER);",
        "CREATE TABLE t (a VARCHAR(-1));", "CREATE TABLE t (a INT DEFAULT 1 NOT NULL);",
        "CREATE TABLE t (a INT, UNIQUE (a));",
        "CREATE TABLE t (a INT, FOREIGN KEY (a) REFERENCES p ());",
        "ALTER TABLE t DROP INDEX;", "ALTER TABLE t DROP FOREIGN KEY;",
        "ALTER TABLE t ADD INDEX idx ();", "ALTER TABLE t ADD CONSTRAINT uq UNIQUE (a);",
        "LOAD DATA INFILE \"x\" INTO TABLE t FIELDS TERMINATED BY ',';",
        "--;", "NULL", ";", "exit;", "USE school; -- comment;", "select * FROM t;"
    })
    {
        bool rejected = false;
        try { auto ast = facade.parse_one(sql); }
        catch(const db_error& error)
        {
            rejected = true;
            CHECK(error.code() == error_code::syntax_error);
            CHECK(error.context().operation == "parse_one");
            CHECK(error.context().line && error.context().column);
            CHECK(*error.context().line >= 1 && *error.context().column >= 1);
        }
        if(!rejected)
            throw std::runtime_error("invalid SQL was accepted: " + std::string(sql));
        auto next = facade.parse_one("USE db;");
        CHECK(as<use_database_statement>(next).name.text == "db");
    }

    std::istringstream input("-- comment; USE db; INSERT INTO t VALUES ('a;b');\nNULL;\nexit\n");
    statement_reader reader(input);
    auto use_sql = reader.next();
    CHECK(use_sql.has_value());
    as<use_database_statement>(facade.parse_one(*use_sql));
    auto insert_sql = reader.next();
    CHECK(insert_sql.has_value());
    auto insert = facade.parse_one(*insert_sql);
    CHECK(as<insert_statement>(insert).rows[0][0]->text == "'a;b'");
    auto null_sql = reader.next();
    CHECK(null_sql.has_value());
    as<noop_statement>(facade.parse_one(*null_sql));
    CHECK(!reader.next());
}
} // namespace

int main()
{
    try
    {
        simple_commands();
        insert_and_load();
        update_and_delete();
        column_definitions();
        create_keys();
        alter_add();
        alter_drop();
        bounded_input_and_ownership();
        multiline_literal_positions();
        positioned_statement_errors();
        reader_script_and_parser_recovery();
        invalid_statements_and_reader();
        std::cout << "Statement AST tests passed\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
