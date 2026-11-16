#pragma once

#include <cstddef>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace db
{

// ---------- Owned syntax data; no catalog IDs or borrowed token pointers ----------

struct source_position
{
    // One-based position within the SQL passed to parser::parse_one.
    std::size_t line = 1;
    std::size_t column = 1;
};

struct sql_name
{
    std::string text;
    source_position position;
};

struct create_database_statement
{
    sql_name name;
};

struct drop_database_statement
{
    sql_name name;
};

struct use_database_statement
{
    sql_name name;
};

struct show_databases_statement
{
};

struct noop_statement
{
};

// Initial AST covers database commands and NULL; only.
// Add table/DML and unsupported-feature nodes before implementing their conversion.
// Never map an unimplemented SQL statement to noop_statement.
typedef std::variant<
    create_database_statement,
    drop_database_statement,
    use_database_statement,
    show_databases_statement,
    noop_statement
> statement;

class statement_reader
{
private:
    // Borrow the stream; its owner must keep it alive until this reader is destroyed.
    std::istream& input_;

    // Future next() implementation must retain any unconsumed input and lexical state.
    // Select those fields when implementing splitting; do not seek the input stream.

public:
    // Store the stream reference without reading input or accessing database files.
    explicit statement_reader(std::istream& input);

    // Planned: return one owned, semicolon-terminated SQL statement; nullopt means normal end.
    // Incomplete input -> syntax_error; stream failure -> io_error. Currently a throwing stub.
    std::optional<std::string> next();

    statement_reader(const statement_reader&) = delete;
    statement_reader& operator=(const statement_reader&) = delete;
    statement_reader(statement_reader&&) = delete;
    statement_reader& operator=(statement_reader&&) = delete;
};

class parser
{
public:
    // Planned: borrow sql only during this call and return an independent AST.
    // Require exactly one complete statement; syntax errors carry one-based line/column.
    // No catalog lookup, execution or output. Currently a throwing stub.
    statement parse_one(std::string_view sql) const;
};

}
