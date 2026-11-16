#pragma once

#include <db/common/types.h>
#include <cstddef>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace db
{

struct source_position
{
    std::size_t line = 1;
    std::size_t column = 1;
};

struct sql_name
{
    std::string text;
    source_position position;
};

enum class command_type
{
    create_database,
    drop_database,
    use_database,
    show_databases,
    show_tables,
    show_indexes,
    drop_table,
    describe_table,
    noop
};

struct command_statement
{
    command_type type;
    // Named commands require a name; SHOW and noop have none.
    std::optional<sql_name> name;
};

enum class literal_type
{
    integer,
    floating,
    string,
    null
};

struct literal
{
    literal_type type;
    // Original token text, including string quotes; binder converts it to db::value.
    std::string text;
    source_position position;
};

struct column_reference
{
    std::optional<sql_name> table;
    sql_name column;
};

struct select_statement;

typedef std::variant<
    std::monostate,
    literal,
    column_reference,
    std::vector<literal>,
    std::unique_ptr<select_statement>
> predicate_operand;

enum class predicate_type
{
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    is_null,
    is_not_null,
    in,
    like
};

struct predicate
{
    column_reference left;
    predicate_type type;
    // NULL tests use monostate; IN accepts a value list or subquery.
    predicate_operand right;
};

enum class aggregate_type
{
    none,
    count,
    average,
    maximum,
    minimum,
    sum
};

struct selector
{
    aggregate_type aggregate = aggregate_type::none;
    // A missing column is valid only for COUNT(*).
    std::optional<column_reference> column;
};

struct order_clause
{
    column_reference column;
    bool descending = false;
};

struct limit_clause
{
    literal count;
    std::optional<literal> offset;
};

struct column_declaration
{
    sql_name name;
    data_type type;
    // Only VARCHAR has a length token; range validation belongs to binder.
    std::optional<literal> max_length;
    bool nullable = true;
    // Missing DEFAULT differs from DEFAULT NULL.
    std::optional<literal> default_value;
};

enum class key_type
{
    index,
    primary_key,
    foreign_key,
    unique
};

struct foreign_reference
{
    sql_name table;
    std::vector<sql_name> columns;
};

struct key_declaration
{
    key_type type;
    std::optional<sql_name> name;
    std::vector<sql_name> columns;
    // Only a foreign-key definition has a REFERENCES clause.
    std::optional<foreign_reference> reference;
};

typedef std::variant<column_declaration,key_declaration> field_declaration;

struct create_table_statement
{
    sql_name name;
    // Preserve the order of columns and table constraints in field_list.
    std::vector<field_declaration> fields;
};

struct insert_statement
{
    sql_name table;
    std::vector<std::vector<literal>> rows;
};

struct select_statement
{
    std::vector<sql_name> tables;
    bool select_all = false;
    std::vector<selector> selectors;
    // Conditions combine using AND; an empty list means no WHERE clause.
    std::vector<predicate> conditions;
    std::optional<column_reference> group_by;
    std::optional<order_clause> order_by;
    std::optional<limit_clause> limit;
};

struct assignment
{
    sql_name column;
    literal replacement;
};

struct update_statement
{
    sql_name table;
    std::vector<assignment> assignments;
    std::vector<predicate> conditions;
};

struct delete_statement
{
    sql_name table;
    std::vector<predicate> conditions;
};

struct load_statement
{
    sql_name table;
    literal path;
    literal delimiter;
};

enum class alter_action
{
    add,
    drop
};

struct alter_table_statement
{
    sql_name table;
    alter_action action;
    // ADD supplies a definition; DROP supplies only the key kind and optional name.
    key_declaration key;
};

typedef std::variant<
    command_statement,
    create_table_statement,
    insert_statement,
    select_statement,
    update_statement,
    delete_statement,
    load_statement,
    alter_table_statement
> statement;

class statement_reader
{
private:
    enum class lexical_state
    {
        normal,
        string,
        comment
    };

    enum class reader_state
    {
        active,
        exhausted,
        failed
    };

    std::istream& input_;
    std::string line_;
    std::size_t offset_ = 0;
    std::string pending_;
    lexical_state lexical_state_ = lexical_state::normal;
    reader_state state_ = reader_state::active;

public:
    explicit statement_reader(std::istream& input);

    std::optional<std::string> next();

    statement_reader(const statement_reader&) = delete;
    statement_reader& operator=(const statement_reader&) = delete;
    statement_reader(statement_reader&&) = delete;
    statement_reader& operator=(statement_reader&&) = delete;
};

class parser
{
public:
    statement parse_one(std::string_view sql) const;
};

}
