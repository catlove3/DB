#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <antlr4-runtime.h>
#include <SQLLexer.h>
#include <SQLParser.h>
#include <SQLBaseVisitor.h>

namespace db
{

// ---------- Source records: owned spelling and one-based positions ----------

struct source_position
{
    std::size_t line = 1;
    std::size_t column = 1;
    void set(antlr4::Token * token);
    source_position(antlr4::Token * token);
    source_position() = default;
    static source_position at_keyword(antlr4::ParserRuleContext* ctx, std::string_view keyword);
};

struct sql_name
{
    std::string text;
    source_position position;
    sql_name(antlr4::tree::TerminalNode* node);
    sql_name() = default;
    static std::vector<sql_name> from_list(SQLParser::IdentifiersContext* ctx);
};

struct column_reference
{
    std::optional<sql_name> table;
    sql_name column;
    column_reference(SQLParser::ColumnContext* ctx);
    column_reference() = default;
};

class ast_visitor;
struct select_statement;

template<class Node>
using node_ptr = std::unique_ptr<Node>;

template<class Node>
using node_list = std::vector<node_ptr<Node>>;

// ---------- AST ownership and visitor dispatch ----------

// Move-only syntax records: conversion fills public fields before publishing a tree.
// Required pointers and list entries must be non-null in a successful parse result.
// Concrete optional children use optional<T>; polymorphic ones use node_ptr<T>.
// Conversion members require valid ANTLR contexts; parse_one performs syntax validation.
class ast_node
{
protected:
    ast_node() = default;
    ast_node(ast_node&&) noexcept = default;
    ast_node& operator=(ast_node&&) noexcept = default;

public:
    source_position position;

    virtual ~ast_node() = default;
    virtual void accept(ast_visitor& visitor) const = 0;

    ast_node(const ast_node&) = delete;
    ast_node& operator=(const ast_node&) = delete;
};

namespace detail
{

// CRTP supplies the same double dispatch for every final syntax node.
// Only Derived may construct/move this base, so the downcast cannot be
// introduced accidentally by an unrelated derived class.
template<class Derived, class Base>
class ast_leaf : public Base
{
    static_assert(std::is_base_of_v<ast_node, Base>);
    friend Derived;

    ast_leaf() = default;
    ast_leaf(ast_leaf&&) noexcept = default;
    ast_leaf& operator=(ast_leaf&&) noexcept = default;

public:
    ~ast_leaf() override = default;
    void accept(ast_visitor& visitor) const final;
};

} // namespace detail

// Category bases share syntax fields; only final leaves dispatch to visitors.
class statement : public ast_node
{
public:
    static node_ptr<statement> from_parse(SQLParser::StatementContext* ctx);
    static node_ptr<statement> from_parse(SQLParser::Db_statementContext* ctx);
    static node_ptr<statement> from_parse(SQLParser::Table_statementContext* ctx);
    static node_ptr<statement> from_parse(SQLParser::Alter_statementContext* ctx);
};

class expression : public ast_node
{
public:
    static node_ptr<expression> from_parse(SQLParser::ExpressionContext* ctx);
    static node_ptr<expression> from_selector(SQLParser::SelectorContext* ctx);
};

class predicate : public ast_node
{
public:
    column_reference left;
    static node_ptr<predicate> from_parse(SQLParser::Where_clauseContext* ctx);
    // A null context denotes an omitted WHERE and produces an empty list.
    static node_list<predicate> from_list(SQLParser::Where_and_clauseContext* ctx);
};

class field_declaration : public ast_node
{
public:
    static node_ptr<field_declaration> from_parse(SQLParser::FieldContext* ctx);
};

class named_statement : public statement
{
public:
    sql_name name;
};

class table_statement : public statement
{
public:
    sql_name table;
};

// ---------- Commands ----------

struct create_database_statement final
    : detail::ast_leaf<create_database_statement, named_statement>
{
    static node_ptr<create_database_statement> from_parse(SQLParser::Create_dbContext* ctx);
};
struct drop_database_statement final
    : detail::ast_leaf<drop_database_statement, named_statement>
{
    static node_ptr<drop_database_statement> from_parse(SQLParser::Drop_dbContext* ctx);
};
struct use_database_statement final
    : detail::ast_leaf<use_database_statement, named_statement>
{
    static node_ptr<use_database_statement> from_parse(SQLParser::Use_dbContext* ctx);
};

// All single-table operations expose their target through table_statement.
struct drop_table_statement final
    : detail::ast_leaf<drop_table_statement, table_statement>
{
    static node_ptr<drop_table_statement> from_parse(SQLParser::Drop_tableContext* ctx);
};
struct describe_table_statement final
    : detail::ast_leaf<describe_table_statement, table_statement>
{
    static node_ptr<describe_table_statement> from_parse(SQLParser::Describe_tableContext* ctx);
};

struct show_databases_statement final
    : detail::ast_leaf<show_databases_statement, statement>
{
    static node_ptr<show_databases_statement> from_parse(SQLParser::Show_dbsContext* ctx);
};
struct show_tables_statement final
    : detail::ast_leaf<show_tables_statement, statement>
{
    static node_ptr<show_tables_statement> from_parse(SQLParser::Show_tablesContext* ctx);
};
struct show_indexes_statement final
    : detail::ast_leaf<show_indexes_statement, statement>
{
    static node_ptr<show_indexes_statement> from_parse(SQLParser::Show_indexesContext* ctx);
};
struct noop_statement final
    : detail::ast_leaf<noop_statement, statement>
{
    static node_ptr<noop_statement> from_parse(SQLParser::StatementContext* ctx);
};

// ---------- Expressions ----------

class literal : public expression
{
public:
    // Preserve the original token, including quotes, until binding.
    std::string text;
    static node_ptr<literal> from_parse(SQLParser::ValueContext* ctx);
};

struct integer_literal final : detail::ast_leaf<integer_literal, literal> {};
struct floating_literal final : detail::ast_leaf<floating_literal, literal> {};
struct string_literal final : detail::ast_leaf<string_literal, literal> {};
struct null_literal final : detail::ast_leaf<null_literal, literal> {};

struct column_expression final : detail::ast_leaf<column_expression, expression>
{
    column_reference column;
    explicit column_expression(SQLParser::ColumnContext* ctx);
};

struct wildcard_expression final
    : detail::ast_leaf<wildcard_expression, expression> {};

struct subquery_expression final : detail::ast_leaf<subquery_expression, expression>
{
    node_ptr<select_statement> query;
    static node_ptr<subquery_expression> from_parse(SQLParser::Select_tableContext* ctx);
};

struct value_list_expression final : detail::ast_leaf<value_list_expression, expression>
{
    node_list<literal> values;
    static node_ptr<value_list_expression> from_parse(SQLParser::Value_listContext* ctx);
};

class aggregate_expression : public expression
{
public:
    // A column_expression, or wildcard_expression for COUNT(*) only.
    node_ptr<expression> argument;
};

struct count_expression final : detail::ast_leaf<count_expression, aggregate_expression> {};
struct average_expression final : detail::ast_leaf<average_expression, aggregate_expression> {};
struct maximum_expression final : detail::ast_leaf<maximum_expression, aggregate_expression> {};
struct minimum_expression final : detail::ast_leaf<minimum_expression, aggregate_expression> {};
struct sum_expression final : detail::ast_leaf<sum_expression, aggregate_expression> {};

// ---------- WHERE predicates ----------

class comparison_predicate : public predicate
{
public:
    // A literal, column_expression or subquery_expression, validated by conversion.
    node_ptr<expression> right;

    // Create the operator's node type; the caller fills operands and position.
    static node_ptr<comparison_predicate> from_operator(SQLParser::Operator_Context* ctx);
};

struct equal_predicate final : detail::ast_leaf<equal_predicate, comparison_predicate> {};
struct not_equal_predicate final : detail::ast_leaf<not_equal_predicate, comparison_predicate> {};
struct less_predicate final : detail::ast_leaf<less_predicate, comparison_predicate> {};
struct less_equal_predicate final : detail::ast_leaf<less_equal_predicate, comparison_predicate> {};
struct greater_predicate final : detail::ast_leaf<greater_predicate, comparison_predicate> {};
struct greater_equal_predicate final
    : detail::ast_leaf<greater_equal_predicate, comparison_predicate> {};

class null_predicate : public predicate
{
};

struct is_null_predicate final : detail::ast_leaf<is_null_predicate, null_predicate> {};
struct is_not_null_predicate final : detail::ast_leaf<is_not_null_predicate, null_predicate> {};

struct in_predicate final : detail::ast_leaf<in_predicate, predicate>
{
    // A value_list_expression or subquery_expression; never null after parsing.
    node_ptr<expression> right;
};

struct like_predicate final : detail::ast_leaf<like_predicate, predicate>
{
    string_literal pattern;
};

// ---------- Query clauses ----------

class order_clause : public ast_node
{
public:
    column_reference column;
};

struct ascending_order final : detail::ast_leaf<ascending_order, order_clause> {};
struct descending_order final : detail::ast_leaf<descending_order, order_clause> {};

struct limit_clause final : detail::ast_leaf<limit_clause, ast_node>
{
    integer_literal count;
    std::optional<integer_literal> offset;
};

// ---------- Table fields and constraints ----------

class type_specification : public ast_node
{
public:
    static node_ptr<type_specification> from_parse(SQLParser::Type_Context* ctx);
};

struct integer_type final : detail::ast_leaf<integer_type, type_specification> {};
struct floating_type final : detail::ast_leaf<floating_type, type_specification> {};

struct varchar_type final : detail::ast_leaf<varchar_type, type_specification>
{
    integer_literal max_length;
};

class column_attribute : public ast_node
{
};

struct not_null_attribute final : detail::ast_leaf<not_null_attribute, column_attribute> {};

struct default_attribute final : detail::ast_leaf<default_attribute, column_attribute>
{
    // DEFAULT NULL owns a null_literal; absence of this attribute means no DEFAULT.
    node_ptr<literal> value;
};

struct column_declaration final : detail::ast_leaf<column_declaration, field_declaration>
{
    sql_name name;
    node_ptr<type_specification> type;
    // SQL.g4 permits each attribute once, with NOT NULL before DEFAULT.
    std::optional<not_null_attribute> not_null;
    std::optional<default_attribute> default_value;
    static node_ptr<column_declaration> from_parse(SQLParser::Normal_fieldContext* ctx);
};

class key_declaration : public field_declaration
{
public:
    // Position starts at INDEX/PRIMARY/FOREIGN/UNIQUE, including for ALTER ADD.
    std::optional<sql_name> name;
    std::vector<sql_name> columns;
};

struct index_declaration final : detail::ast_leaf<index_declaration, key_declaration>
{
    static node_ptr<index_declaration> from_parse(SQLParser::Alter_add_indexContext* ctx);
};
struct primary_key_declaration final
    : detail::ast_leaf<primary_key_declaration, key_declaration>
{
    static node_ptr<primary_key_declaration> from_parse(SQLParser::Primary_key_fieldContext* ctx);
    static node_ptr<primary_key_declaration> from_parse(SQLParser::Alter_table_add_pkContext* ctx);
};

struct foreign_key_declaration final : detail::ast_leaf<foreign_key_declaration, key_declaration>
{
    sql_name parent_table;
    std::vector<sql_name> parent_columns;
    static node_ptr<foreign_key_declaration> from_parse(SQLParser::Foreign_key_fieldContext* ctx);
    static node_ptr<foreign_key_declaration> from_parse(SQLParser::Alter_table_add_foreign_keyContext* ctx);
};

struct unique_declaration final : detail::ast_leaf<unique_declaration, key_declaration>
{
    static node_ptr<unique_declaration> from_parse(SQLParser::Alter_table_add_uniqueContext* ctx);
};

// ---------- Structured statements ----------

struct create_table_statement final : detail::ast_leaf<create_table_statement, table_statement>
{
    // Preserve the original order of columns and table constraints.
    node_list<field_declaration> fields;
    static node_ptr<create_table_statement> from_parse(SQLParser::Create_tableContext* ctx);
};

struct insert_statement final : detail::ast_leaf<insert_statement, table_statement>
{
    std::vector<node_list<literal>> rows;
    static node_ptr<insert_statement> from_parse(SQLParser::Insert_into_tableContext* ctx);
};

struct select_statement final : detail::ast_leaf<select_statement, statement>
{
    std::vector<sql_name> tables;
    // SELECT * has exactly one wildcard_expression; otherwise use columns/aggregates.
    node_list<expression> selectors;
    // WHERE combines predicates with AND; an empty list means no WHERE clause.
    node_list<predicate> conditions;
    std::optional<column_reference> group_by;
    node_ptr<order_clause> order_by;
    std::optional<limit_clause> limit;

    static node_ptr<select_statement> from_parse(SQLParser::Select_tableContext * ctx);
};

struct assignment
{
    sql_name column;
    node_ptr<literal> replacement;
    static assignment from_parse(antlr4::tree::TerminalNode* column, SQLParser::ValueContext* value);
};

struct update_statement final : detail::ast_leaf<update_statement, table_statement>
{
    std::vector<assignment> assignments;
    // UPDATE requires WHERE; unlike SELECT/DELETE this list cannot be empty.
    node_list<predicate> conditions;
    static node_ptr<update_statement> from_parse(SQLParser::Update_tableContext* ctx);
};

struct delete_statement final : detail::ast_leaf<delete_statement, table_statement>
{
    node_list<predicate> conditions;
    static node_ptr<delete_statement> from_parse(SQLParser::Delete_from_tableContext* ctx);
};

struct load_statement final : detail::ast_leaf<load_statement, table_statement>
{
    string_literal path;
    string_literal delimiter;
    static node_ptr<load_statement> from_parse(SQLParser::Load_tableContext* ctx);
};

struct add_key_statement final : detail::ast_leaf<add_key_statement, table_statement>
{
    node_ptr<key_declaration> key;
    static node_ptr<add_key_statement> from_parse(SQLParser::Alter_statementContext* ctx);
};

class drop_key_statement : public table_statement
{
};

struct drop_index_statement final : detail::ast_leaf<drop_index_statement, drop_key_statement>
{
    sql_name name;
    static node_ptr<drop_index_statement> from_parse(SQLParser::Alter_drop_indexContext* ctx);
};

struct drop_primary_key_statement final
    : detail::ast_leaf<drop_primary_key_statement, drop_key_statement>
{
    // Only PRIMARY KEY permits omission of the key name.
    std::optional<sql_name> name;
    static node_ptr<drop_primary_key_statement> from_parse(SQLParser::Alter_table_drop_pkContext* ctx);
};

struct drop_foreign_key_statement final
    : detail::ast_leaf<drop_foreign_key_statement, drop_key_statement>
{
    sql_name name;
    static node_ptr<drop_foreign_key_statement> from_parse(SQLParser::Alter_table_drop_foreign_keyContext* ctx);
};

// ---------- Exhaustive visitor interface ----------

// No default handler: adding a leaf requires each visitor to handle it explicitly.
// accept dispatches one node; traversal and result storage belong to the visitor.
class ast_visitor
{
public:
    virtual ~ast_visitor() = default;

    virtual void visit(const create_database_statement& node) = 0;
    virtual void visit(const drop_database_statement& node) = 0;
    virtual void visit(const use_database_statement& node) = 0;
    virtual void visit(const drop_table_statement& node) = 0;
    virtual void visit(const describe_table_statement& node) = 0;
    virtual void visit(const show_databases_statement& node) = 0;
    virtual void visit(const show_tables_statement& node) = 0;
    virtual void visit(const show_indexes_statement& node) = 0;
    virtual void visit(const noop_statement& node) = 0;
    virtual void visit(const integer_literal& node) = 0;
    virtual void visit(const floating_literal& node) = 0;
    virtual void visit(const string_literal& node) = 0;
    virtual void visit(const null_literal& node) = 0;
    virtual void visit(const column_expression& node) = 0;
    virtual void visit(const wildcard_expression& node) = 0;
    virtual void visit(const subquery_expression& node) = 0;
    virtual void visit(const value_list_expression& node) = 0;
    virtual void visit(const count_expression& node) = 0;
    virtual void visit(const average_expression& node) = 0;
    virtual void visit(const maximum_expression& node) = 0;
    virtual void visit(const minimum_expression& node) = 0;
    virtual void visit(const sum_expression& node) = 0;
    virtual void visit(const equal_predicate& node) = 0;
    virtual void visit(const not_equal_predicate& node) = 0;
    virtual void visit(const less_predicate& node) = 0;
    virtual void visit(const less_equal_predicate& node) = 0;
    virtual void visit(const greater_predicate& node) = 0;
    virtual void visit(const greater_equal_predicate& node) = 0;
    virtual void visit(const is_null_predicate& node) = 0;
    virtual void visit(const is_not_null_predicate& node) = 0;
    virtual void visit(const in_predicate& node) = 0;
    virtual void visit(const like_predicate& node) = 0;
    virtual void visit(const ascending_order& node) = 0;
    virtual void visit(const descending_order& node) = 0;
    virtual void visit(const limit_clause& node) = 0;
    virtual void visit(const integer_type& node) = 0;
    virtual void visit(const floating_type& node) = 0;
    virtual void visit(const varchar_type& node) = 0;
    virtual void visit(const not_null_attribute& node) = 0;
    virtual void visit(const default_attribute& node) = 0;
    virtual void visit(const column_declaration& node) = 0;
    virtual void visit(const index_declaration& node) = 0;
    virtual void visit(const primary_key_declaration& node) = 0;
    virtual void visit(const foreign_key_declaration& node) = 0;
    virtual void visit(const unique_declaration& node) = 0;
    virtual void visit(const create_table_statement& node) = 0;
    virtual void visit(const insert_statement& node) = 0;
    virtual void visit(const select_statement& node) = 0;
    virtual void visit(const update_statement& node) = 0;
    virtual void visit(const delete_statement& node) = 0;
    virtual void visit(const load_statement& node) = 0;
    virtual void visit(const add_key_statement& node) = 0;
    virtual void visit(const drop_index_statement& node) = 0;
    virtual void visit(const drop_primary_key_statement& node) = 0;
    virtual void visit(const drop_foreign_key_statement& node) = 0;
};

template<class Derived, class Base>
void detail::ast_leaf<Derived, Base>::accept(ast_visitor& visitor) const
{
    static_assert(std::is_final_v<Derived>);
    visitor.visit(static_cast<const Derived&>(*this));
}

// ---------- Input boundary ----------

class statement_reader
{
private:
    enum class lexical_state
    {
        normal,
        quoted_string,
        comment
    };

    enum class reader_state
    {
        active,
        exhausted,
        failed
    };

    std::istream& input_;
    std::size_t offset_ = 0;
    std::string line_;
    std::string pending_;
    lexical_state lexical_state_ = lexical_state::normal;
    bool comment_has_content_ = false;
    reader_state state_ = reader_state::active;

    // true: continue collecting; false: a statement is ready or input has ended.
    bool read_next_token();

    statement_reader(const statement_reader&) = delete;
    statement_reader& operator=(const statement_reader&) = delete;
    statement_reader(statement_reader&&) = delete;
    statement_reader& operator=(statement_reader&&) = delete;
public:
    // Borrow input for this reader's lifetime; construction performs no reads.
    explicit statement_reader(std::istream& input) noexcept;

    // Owned, semicolon-terminated SQL; nullopt means normal EOF or standalone exit.
    // Unfinished input raises syntax_error; a stream failure raises io_error.
    [[nodiscard]] std::optional<std::string> next();
};

class parser
{
public:
    // Exactly one complete SQL statement; borrow sql only until return.
    // Success owns all text/subtrees; syntax_error includes one-based line/column.
    // All SQL.g4 statements are converted; binding and execution happen separately.
    [[nodiscard]] node_ptr<statement> parse_one(std::string_view sql) const;
};

} // namespace db
