#include <db/parser/parser.h>
#include <db/common/error.h>
#include <istream>
#include <new>

namespace db
{

namespace
{
    std::string_view strip(std::string_view str) noexcept
    {
        // Match SQL.g4 whitespace without copying or classifying signed chars.
        const auto first = str.find_first_not_of(" \t\r\n");
        if(first == std::string_view::npos)
            return {};
        const auto last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }

    [[noreturn]] void statement_error(error_code code, const char* message)
    {
        error_context context;
        context.operation = "read_statement";
        throw db_error(code, message, context);
    }

    bool read_line(std::istream& input, std::string& line)
    {
        try
        {
            std::getline(input, line);
        }
        catch(const std::bad_alloc&)
        {
            throw;
        }
        catch(...)
        {
            // An exception mask can make normal EOF throw, even after getline
            // extracted a final line. A streambuf failure sets badbit instead.
            if(input.bad() || !input.eof())
                statement_error(error_code::io_error, "failed to read input stream");
        }

        // A real error takes precedence even when eofbit is also set.
        if(input.bad() || (input.fail() && !input.eof()))
            statement_error(error_code::io_error, "failed to read input stream");
        if(input.fail())
            return false; // EOF without a line to process.

        if(!input.eof())
            line.push_back('\n'); // Restore only a delimiter actually consumed.
        return true;
    }
}
/*
    Implementation status:
        AST records, ownership aliases and shared Visitor dispatch are defined in parser.h.
        statement_reader implements splitting, standalone comments and shell exit handling.
        parser::parse_one remains an interface contract; the lexer, parse-tree conversion
        and ANTLR build integration are not implemented.
*/

/*
    CLASS : AST NODE
    docs:
        Use a Composite of syntax nodes with unique ownership. Category bases share fields;
        final leaves identify SQL operations. Keep binding and execution in separate visitors.
        sql_name, column_reference and assignment are data records, not dispatch targets.
        AST types do not depend on catalog types, db::value or generated ANTLR headers.

    layout:
        position        : One-based line/column of the node's first token in the supplied SQL.
        named_statement : Database name for CREATE DATABASE, DROP DATABASE and USE.
        table_statement : Target table for all single-table commands, DDL and mutations,
                          including DROP TABLE and DESC. SELECT has its own source list.
        literal         : Original token spelling; string tokens retain their quotes.
        predicate       : Left column shared by every SQL.g4 WHERE condition.
        key_declaration : Optional key name and ordered local column names.

    ownership:
        node_ptr<T> is unique_ptr<T>; node_list<T> is a vector of those owning pointers.
        Polymorphic children and recursive SELECT trees use node_ptr. Required pointers
        and list elements must be non-null after conversion; an absent ORDER BY is null.
        Concrete required children are values; concrete optional children use optional<T>.
        LIMIT, OFFSET and column attributes need no separate allocation for their wrappers.
        Names and token spellings own strings; no AST field borrows input or ANTLR memory.
        Nodes cannot be copied. Move a concrete value or transfer its owning pointer;
        protected base moves prevent assigning only the base part through an AST reference.
        Virtual destruction releases children through their correct concrete types.

    construction:
        Public fields are construction records, not a general validation API. Conversion
        fills them before publishing a tree and enforces nonempty lists, required children
        and legal expression categories. A default-constructed node need not be valid SQL.
        std::optional distinguishes omitted syntax from a present clause. In particular,
        a present default_attribute owns a literal even when that literal is NULL.
        Allocation failures propagate; owned intermediate nodes release their subtrees.
*/

/*
    CLASS : AST LEAF / AST VISITOR
    docs:
        detail::ast_leaf<Derived, Base> is the CRTP dispatch mixin for final syntax nodes.
        It implements accept once in the header, after ast_visitor is fully declared.
        Only Derived can construct or move its mixin; Base must derive from ast_node.
        Abstract category bases do not use the mixin and have no visit overload.

    dispatch:
        A const ast_node reference calls virtual accept. The mixin casts to its final
        Derived type and calls visitor.visit, selecting the exact concrete overload.
        Virtual visit then reaches the caller's operation. No SQL kind enums, variant
        switches, dynamic_cast chains or per-leaf accept definitions are needed.
        accept dispatches only this node, without visiting children or storing results.
        Traversal order and result storage belong to the concrete visitor.

    extension:
        Add a final node derived from ast_leaf<NewNode, Category>, then add its pure visit
        overload to ast_visitor. Every concrete visitor must handle the new leaf explicitly.
        The exhaustive interface intentionally has no silent default implementation.
        COUNT/AVG/MAX/MIN/SUM, the six comparisons and ASC/DESC remain distinct targets.

    errors:
        accept propagates visitor exceptions unchanged; it performs no allocation itself.
        Visitors inspect const nodes. Neither dispatch nor destruction performs SQL work,
        accesses storage or prints diagnostics.
*/

/*
    SQL AST : GRAMMAR SHAPES
    expressions:
        integer/floating/string/null_literal preserve spelling and source position.
        column_expression contains a qualified or unqualified column_reference.
        wildcard_expression represents SELECT * or the argument of COUNT(*).
        subquery_expression owns a non-null SELECT; value_list_expression owns a nonempty
        ordered list of literals. An aggregate owns a column_expression argument, except
        COUNT may instead own a wildcard_expression. AVG/MAX/MIN/SUM cannot use '*'.

    predicates:
        comparison_predicate owns a non-null literal, column_expression or subquery_expression.
        in_predicate owns a nonempty value_list_expression or a subquery_expression.
        is_null/is_not_null have no right operand; like_predicate has a string_literal pattern.
        Predicates in conditions combine with AND and preserve their input order.
        Expression-category restrictions are checked during conversion, not by node_ptr.

    declarations:
        A column has a name and non-null integer_type, floating_type or varchar_type.
        VARCHAR owns its integer length token. Numeric ranges belong to binding.
        not_null and default_value are separate optionals, so each attribute can occur at
        most once. SQL.g4 fixes their order: NOT NULL, then DEFAULT. No not_null means
        nullable; no default_value means no DEFAULT; DEFAULT NULL owns a null_literal.
        A key has nonempty local columns. Foreign keys also have a parent table and
        nonempty parent columns. Column counts and integrity constraints belong to binding.
        CREATE TABLE fields preserve order and contain columns, primary keys or foreign
        keys only. INDEX and UNIQUE declarations are accepted only by ALTER TABLE ADD.

    statements:
        create/drop/use_database use named_statement::name. DROP TABLE, DESC, CREATE TABLE,
        INSERT, UPDATE, DELETE, LOAD and all ALTER operations use table_statement::table.
        SHOW DATABASES/TABLES/INDEXES and noop_statement have no payload beyond position.
        INSERT contains nonempty rows of nonempty literal lists, in input order.
        SELECT contains nonempty tables and selectors. SELECT * has exactly one wildcard;
        other projections contain columns or aggregates. WHERE, GROUP BY, ORDER BY and
        LIMIT/OFFSET are optional and follow that order. OFFSET requires LIMIT.
        An omitted ORDER direction and explicit ASC both produce ascending_order;
        an absent ORDER BY has no node. LIMIT and OFFSET retain integer tokens.
        UPDATE has nonempty assignments and mandatory WHERE conditions; DELETE permits
        empty conditions, meaning all rows. LOAD owns string path and delimiter tokens.
        ADD owns a non-null index/primary/foreign/unique declaration. DROP INDEX and
        DROP FOREIGN KEY require sql_name fields; only DROP PRIMARY KEY has optional name.
        Standalone NULL; and Annotation produce noop_statement. A NULL value is a literal.

    boundary:
        These are syntax records. Name resolution, type conversion, SQL NULL semantics,
        subquery cardinality and execution support checks belong to binding/execution.
        Preserve the grammar's uppercase keywords and all-column INSERT syntax.
*/

/*
    CLASS : STATEMENT READER
    docs:
        Split an input stream into complete SQL statements before parsing.
        Borrow input for the reader's lifetime; the caller keeps it alive.
        Never close or seek the stream, access database files or print output.

    layout:
        input_         : Borrowed source stream.
        line_          : Current input line, retaining its newline if one was consumed.
        offset_        : Next unread byte in line_; retain the suffix after returning SQL.
        pending_       : SQL accumulated across lines, preserving original whitespace.
        lexical_state_ : Normal text, quoted_string or comment.
        comment_has_content_ : Whether the current comment has a character after --.
        state_         : Active, normally exhausted or failed.

    methods:
        statement_reader : Store input with empty buffers; construction does not read or throw.
        next             : Return one owned string including its terminating semicolon.
                           Continue across lines; retain later statements for subsequent calls.
                           Skip whitespace and complete standalone comments.
                           EOF with no unfinished SQL returns nullopt; later calls do too.

    parsing:
        Follow grammar/SQL.g4: single-quoted strings may contain newlines and semicolons.
        The grammar defines neither backslash escapes nor doubled-quote escapes.
        A -- comment ends at its semicolon, not at a newline. Annotation requires at
        least one character after --; splitting must not silently accept an empty --;.
        An independent line equal to exit after trimming surrounding whitespace requests
        normal termination only outside strings/comments and with no pending SQL.
        exit inside a string or an unfinished statement remains part of that input.

    errors:
        EOF with unfinished SQL, string or comment -> syntax_error.
        Actual stream read failure -> io_error with operation=read_statement.
        Honor the stream's exception mask without changing it; EOF alone is not an I/O error.
        Do not turn read failures into normal EOF or publish truncated SQL.
        After failure, stop using this reader; consumed input cannot be rolled back.
        Allocation failures propagate; no database or catalog state is modified.
*/

statement_reader::statement_reader(std::istream& input) noexcept : input_(input)
{
}


bool statement_reader::read_next_token()
{
    if(state_ != reader_state::active)
        return false;

    while(offset_ >= line_.size())
    {
        if(!read_line(input_, line_))
        {
            if(!strip(pending_).empty() || lexical_state_ != lexical_state::normal)
                statement_error(error_code::syntax_error, "unfinished SQL input");
            pending_.clear();
            state_ = reader_state::exhausted;
            return false;
        }
        offset_ = 0;

        if(lexical_state_ == lexical_state::normal &&
           strip(pending_).empty() && strip(line_) == "exit")
        {
            pending_.clear();
            state_ = reader_state::exhausted;
            return false;
        }
    }


    char next_token = line_[offset_];
    if(lexical_state_ == lexical_state::normal)
    {
        if(next_token == '-' && offset_ + 1 < line_.size() && line_[offset_ + 1] == '-')
        {
            if(!strip(pending_).empty())
                statement_error(error_code::syntax_error, "comment appears within SQL statement");

            pending_.clear(); // Discard whitespace before a standalone comment.
            comment_has_content_ = false;
            lexical_state_ = lexical_state::comment;
            offset_ += 2;
            return true;
        }
        else if(next_token == '\'')
            lexical_state_ = lexical_state::quoted_string;
        else if(next_token == ';')
        {
            offset_ += 1;
            pending_.push_back(next_token);
            return false;
        }

        pending_.push_back(next_token);
        offset_ += 1;
        return true;
    }
    else if(lexical_state_ == lexical_state::quoted_string)
    {
        if(next_token == '\'')
            lexical_state_ = lexical_state::normal;
        offset_ += 1;
        pending_.push_back(next_token);
        return true;
    }
    else if(lexical_state_ == lexical_state::comment)
    {
        if(next_token == ';')
        {
            if(!comment_has_content_)
                statement_error(error_code::syntax_error, "empty comment is not allowed by SQL.g4");
            lexical_state_ = lexical_state::normal;
            offset_ += 1;
            return true;
        }
        comment_has_content_ = true;
        offset_ += 1;
        return true;
    }
    else
        statement_error(error_code::internal_error, "unknown lexical state");
}

std::optional<std::string> statement_reader::next()
{
    if(state_ == reader_state::failed)
        statement_error(error_code::syntax_error, "statement reader cannot be reused after failure");

    if(state_ == reader_state::exhausted)
        return std::nullopt;

    try
    {
        while(read_next_token())
        {
        }
    }
    catch(...)
    {
        state_ = reader_state::failed;
        throw;
    }

    if(state_ == reader_state::exhausted)
        return std::nullopt;

    std::string tmp;
    swap(tmp,pending_);
    return tmp;
}




/*
    CLASS : PARSER
    docs:
        Convert exactly one semicolon-terminated statement into an owned AST.
        The parser facade is stateless; per-call input, lexer, tokens, generated parser,
        error listener and AST conversion objects remain implementation details here.
        Input splitting belongs to statement_reader, syntax conversion to parser,
        and name/type resolution to the binding layer.

    method : parse_one
        Borrow sql until return; permit surrounding whitespace. Run the SQL.g4 lexer and
        parser, collect errors from both, and require exactly one statement plus EOF.
        Disable ANTLR's diagnostic printing; report errors through db_error/error_context.
        Do not convert a recovered parse tree if either lexer or parser reported an error.
        Convert every valid grammar branch, including nested SELECTs and all clauses,
        validating the structural invariants above. Return a non-null node_ptr<statement>.
        Move ownership to the caller before destroying per-call parsing objects.
        ANTLR's generated visitor builds this AST; ast_visitor consumes the resulting AST.
        A syntactically valid operation is represented even if execution does not support it.

    errors:
        Empty input, missing terminator, multiple statements or lexical/syntax errors
        -> syntax_error with one-based line/column in error_context.
        Allocation failures propagate; intermediate owning pointers release their trees.
        No partial AST is published and no catalog, table or session state is modified.
        A successful result remains valid after input, lexer and parser destruction.
*/

} // namespace db
