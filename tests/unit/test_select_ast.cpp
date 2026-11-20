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
    auto* typed = dynamic_cast<const T*>(node.get());
    CHECK(typed != nullptr);
    return *typed;
}

// Exercise the public facade, including syntax validation and statement dispatch.
// Returned ASTs must remain usable after all ANTLR objects have been destroyed.
node_ptr<select_statement> parse_select(std::string_view sql)
{
    auto statement = parser{}.parse_one(sql);
    auto* select = dynamic_cast<select_statement*>(statement.get());
    CHECK(select != nullptr);
    statement.release();
    return node_ptr<select_statement>(select);
}

void diagnostics()
{
    struct capture_output
    {
        std::ostringstream out;
        std::ostringstream err;
        std::streambuf* old_out = std::cout.rdbuf(out.rdbuf());
        std::streambuf* old_err = std::cerr.rdbuf(err.rdbuf());

        ~capture_output()
        {
            std::cout.rdbuf(old_out);
            std::cerr.rdbuf(old_err);
        }
    } capture;

    struct error_case
    {
        std::string_view sql;
        error_code code;
        std::size_t line;
        std::size_t column;
    };
    const error_case cases[] = {
        {"", error_code::syntax_error, 1, 1},
        {" \n\t", error_code::syntax_error, 2, 2},
        {"SELECT * FROM t", error_code::syntax_error, 1, 16},
        {"SELECT * FROM t;\n SELECT * FROM u;", error_code::syntax_error, 2, 2},
        {"SELECT *\nFROM ;", error_code::syntax_error, 2, 6},
        {"SELECT *\r\nFROM ;", error_code::syntax_error, 2, 6},
        {"SELECT @ FROM t;", error_code::syntax_error, 1, 8},
        {"SELECT @ # FROM t;", error_code::syntax_error, 1, 8},
        {"SELECT * FROM t WHERE name = 'abc", error_code::syntax_error, 1, 30},
        {"SELECT * FROM t WHERE name = '汉'; @", error_code::syntax_error, 1, 35},
        {"SELECT * FROM t WHERE id IN (SELECT FROM u);", error_code::syntax_error, 1, 37},
        {"USE ;", error_code::syntax_error, 1, 5},
        {" \n USE school;", error_code::feature_unsupported, 2, 2},
        {"USE school;\nSELECT * FROM t;", error_code::syntax_error, 2, 1}
    };

    parser facade;
    for(const auto& item : cases)
    {
        bool rejected = false;
        try
        {
            auto ast = facade.parse_one(item.sql);
        }
        catch(const db_error& error)
        {
            rejected = true;
            CHECK(error.code() == item.code);
            CHECK(error.context().operation == "parse_one");
            CHECK(error.context().line == item.line);
            if(error.context().column != item.column)
                throw std::runtime_error("wrong error column for SQL: " + std::string(item.sql) +
                    "; expected " + std::to_string(item.column) + ", got " +
                    std::to_string(error.context().column.value_or(0)));
            CHECK(!std::string(error.what()).empty());
            CHECK(std::string(error.what()).find("syntax error occurs") == std::string::npos);
        }
        CHECK(rejected);
        // An earlier failure must not leave errors or partial state in the facade.
        auto next = facade.parse_one("SELECT * FROM t;");
        CHECK(as<select_statement>(next).tables.at(0).text == "t");
    }
    CHECK(capture.out.str().empty());
    CHECK(capture.err.str().empty());
}

void projections_and_ownership()
{
    auto wildcard = parse_select("SELECT * FROM students;");
    CHECK(wildcard->selectors.size() == 1);
    CHECK(as<wildcard_expression>(wildcard->selectors[0]).position.column == 8);
    CHECK(wildcard->tables.size() == 1);
    CHECK(wildcard->tables[0].text == "students");
    CHECK(wildcard->tables[0].position.column == 15);
    CHECK(wildcard->conditions.empty());
    CHECK(!wildcard->group_by && !wildcard->order_by && !wildcard->limit);

    std::string sql = "SELECT s.name, age FROM students, teachers;";
    auto query = parse_select(sql);
    sql.assign(20000, 'x');
    CHECK(query->tables.size() == 2);
    CHECK(query->tables[0].text == "students");
    CHECK(query->tables[1].text == "teachers");
    CHECK(query->selectors.size() == 2);
    const auto& name = as<column_expression>(query->selectors[0]);
    CHECK(name.column.table->text == "s");
    CHECK(name.column.column.text == "name");
    CHECK(name.column.table->position.column == 8);
    CHECK(name.column.column.position.column == 10);
    const auto& age = as<column_expression>(query->selectors[1]);
    CHECK(!age.column.table);
    CHECK(age.column.column.text == "age");

    auto aggregates = parse_select(
        "SELECT COUNT(*), COUNT(s.id), AVG(score), MAX(score), MIN(score), SUM(score) FROM s;");
    CHECK(aggregates->selectors.size() == 6);
    const auto& count_all = as<count_expression>(aggregates->selectors[0]);
    CHECK(as<wildcard_expression>(count_all.argument).position.column == 14);
    const auto& count_column = as<count_expression>(aggregates->selectors[1]);
    CHECK(as<column_expression>(count_column.argument).column.table->text == "s");
    CHECK(as<column_expression>(count_column.argument).column.column.text == "id");
    CHECK(as<column_expression>(as<average_expression>(aggregates->selectors[2]).argument)
              .column.column.text == "score");
    as<maximum_expression>(aggregates->selectors[3]);
    as<minimum_expression>(aggregates->selectors[4]);
    as<sum_expression>(aggregates->selectors[5]);
}

void optional_clauses()
{
    auto group = parse_select("SELECT name FROM t GROUP BY name;");
    CHECK(group->group_by->column.text == "name");
    CHECK(!group->order_by && !group->limit);

    auto order = parse_select("SELECT name FROM t ORDER BY t.score;");
    CHECK(!order->group_by);
    CHECK(as<ascending_order>(order->order_by).column.column.text == "score");
    CHECK(order->order_by->column.table->text == "t");
    auto ascending = parse_select("SELECT name FROM t ORDER BY name ASC;");
    as<ascending_order>(ascending->order_by);

    auto both = parse_select(
        "SELECT name FROM t GROUP BY t.department ORDER BY t.score DESC LIMIT 10 OFFSET 2;");
    CHECK(both->group_by->table->text == "t");
    CHECK(both->group_by->column.text == "department");
    CHECK(as<descending_order>(both->order_by).column.column.text == "score");
    CHECK(both->limit->count.text == "10");
    CHECK(both->limit->offset->text == "2");

    auto zero = parse_select("SELECT * FROM t LIMIT 0 OFFSET 0;");
    CHECK(zero->limit->count.text == "0");
    CHECK(zero->limit->offset->text == "0");
    auto large = parse_select("SELECT * FROM t LIMIT 999999999999999999999999;");
    CHECK(large->limit->count.text == "999999999999999999999999");
    CHECK(!large->limit->offset);
    CHECK(!large->group_by && !large->order_by);

    auto lines = parse_select(
        "\n  SELECT name\nFROM t\nGROUP BY name\nORDER BY name DESC\nLIMIT 10 OFFSET 2;");
    CHECK(lines->position.line == 2 && lines->position.column == 3);
    CHECK(lines->tables[0].position.line == 3 && lines->tables[0].position.column == 6);
    CHECK(lines->group_by->column.position.line == 4);
    CHECK(lines->order_by->position.line == 5 && lines->order_by->position.column == 1);
    CHECK(lines->limit->position.line == 6 && lines->limit->position.column == 1);
    CHECK(lines->limit->count.position.column == 7);
    CHECK(lines->limit->offset->position.column == 17);
}

void conditions()
{
    auto query = parse_select(
        "SELECT * FROM t WHERE a = 1 AND b <> 2 AND c < 3 AND d <= 4 "
        "AND e > 5 AND f >= u.f AND g IS NULL AND h IS NOT NULL "
        "AND i IN (1, -2.5, 'x;y', NULL) AND j LIKE 'ORDER GROUP LIMIT%';");
    CHECK(query->conditions.size() == 10);
    CHECK(as<integer_literal>(as<equal_predicate>(query->conditions[0]).right).text == "1");
    as<not_equal_predicate>(query->conditions[1]);
    as<less_predicate>(query->conditions[2]);
    as<less_equal_predicate>(query->conditions[3]);
    as<greater_predicate>(query->conditions[4]);
    const auto& column = as<column_expression>(as<greater_equal_predicate>(query->conditions[5]).right);
    CHECK(column.column.table->text == "u" && column.column.column.text == "f");
    CHECK(as<is_null_predicate>(query->conditions[6]).left.column.text == "g");
    CHECK(as<is_not_null_predicate>(query->conditions[7]).left.column.text == "h");
    const auto& values = as<value_list_expression>(as<in_predicate>(query->conditions[8]).right).values;
    CHECK(values.size() == 4);
    CHECK(as<integer_literal>(values[0]).text == "1");
    CHECK(as<floating_literal>(values[1]).text == "-2.5");
    CHECK(as<string_literal>(values[2]).text == "'x;y'");
    CHECK(as<null_literal>(values[3]).text == "NULL");
    CHECK(as<like_predicate>(query->conditions[9]).pattern.text == "'ORDER GROUP LIMIT%'");
    CHECK(!query->group_by && !query->order_by && !query->limit);
}

void nested_selects()
{
    auto outer = parse_select(
        "SELECT name FROM students WHERE id IN ("
        "SELECT student_id FROM scores WHERE score > ("
        "SELECT AVG(score) FROM scores WHERE class_id = 2) "
        "GROUP BY student_id ORDER BY student_id DESC LIMIT 3 OFFSET 1);");
    CHECK(!outer->group_by && !outer->order_by && !outer->limit);
    const auto& middle = as<subquery_expression>(as<in_predicate>(outer->conditions.at(0)).right).query;
    CHECK(middle->tables[0].text == "scores");
    CHECK(middle->group_by->column.text == "student_id");
    as<descending_order>(middle->order_by);
    CHECK(middle->limit->count.text == "3" && middle->limit->offset->text == "1");
    const auto& inner = as<subquery_expression>(as<greater_predicate>(middle->conditions.at(0)).right).query;
    CHECK(!inner->group_by && !inner->order_by && !inner->limit);
    as<average_expression>(inner->selectors.at(0));
    CHECK(as<integer_literal>(as<equal_predicate>(inner->conditions.at(0)).right).text == "2");

    auto separate = parse_select(
        "SELECT id FROM a WHERE id IN (SELECT id FROM b ORDER BY b.id DESC LIMIT 1) "
        "ORDER BY a.id ASC LIMIT 10 OFFSET 4;");
    CHECK(as<ascending_order>(separate->order_by).column.table->text == "a");
    CHECK(separate->limit->count.text == "10" && separate->limit->offset->text == "4");
    const auto& nested = as<subquery_expression>(as<in_predicate>(separate->conditions.at(0)).right).query;
    CHECK(as<descending_order>(nested->order_by).column.table->text == "b");
    CHECK(nested->limit->count.text == "1" && !nested->limit->offset);
}
} // namespace

int main()
{
    try
    {
        projections_and_ownership();
        optional_clauses();
        conditions();
        nested_selects();
        diagnostics();
        std::cout << "SELECT AST tests passed\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
