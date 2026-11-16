#include <db/parser/parser.h>

namespace db
{

/*
    CLASS : STATEMENT READER
    docs:
        Split an input stream into complete SQL statements before parsing.
        Borrow input for the reader's lifetime; the caller keeps it alive.
        Never close or seek the stream, access database files or print output.

    layout:
        input_         : Borrowed source stream.
        line_          : Current input line, retaining its newline if one was consumed.
        offset_        : Next unread byte in line_; keep the suffix after returning a statement.
        pending_       : SQL accumulated across lines, preserving original whitespace.
        lexical_state_ : Ordinary text, single-quoted string or comment.
        state_         : Active, normally exhausted or failed.

    methods:
        statement_reader : Store input and start with empty buffers; do not read input yet.
        next             : Return one owned SQL string including its terminating semicolon.
                           Continue across lines; retain later statements for subsequent calls.
                           Skip whitespace and complete standalone comments.
                           Normal EOF with no unfinished SQL returns nullopt.
                           After normal end, later calls also return nullopt.

    parsing:
        Follow grammar/SQL.g4: strings use single quotes and may contain newlines/semicolons.
        This grammar defines neither backslash escapes nor doubled-quote escapes.
        A -- comment ends at its semicolon, not at a newline.
        An independent line equal to exit after trimming surrounding whitespace requests
        normal termination only outside strings/comments and with no pending SQL.
        An exit inside a string or unfinished statement remains part of that input.

    errors:
        EOF with unfinished SQL, string or comment -> syntax_error.
        Actual stream read failure -> io_error with operation=read_statement.
        Do not turn a stream failure into normal EOF or publish truncated SQL.
        After failure, stop using this reader; consumed input cannot be rolled back.
        Allocation failures propagate; no database or catalog state is modified.
*/


/*
    CLASS : PARSER
    docs:
        Convert one complete SQL statement into owned syntax data covering grammar/SQL.g4.
        Keep input splitting, AST declarations and parser in parser.h/parser.cpp.
        Reuse common/types.h for shared types; AST-specific structures stay in parser.h.
        Binding, execution and formatting belong to execution.h/execution.cpp.
        No catalog lookup, record access, statement execution or output occurs here.

    layout:
        source_position      : One-based line and column within the supplied SQL.
        sql_name             : Owned identifier text and source position.
        command_statement    : Commands with no argument or one name, distinguished by type.
                               CREATE/DROP/USE DATABASE and DROP/DESC TABLE require a name.
                               SHOW and noop have no name. NULL; and standalone comments are noop.
        literal              : Kind, original token text and source position; keep string quotes.
                               Binder converts tokens to typed values and validates numeric ranges.
        column_reference     : Column name with an optional table qualifier; no persistent IDs.
        predicate            : Comparison, NULL test, IN or LIKE; condition lists combine using AND.
                               Comparisons accept a literal, column or scalar SELECT on the right.
                               NULL tests use monostate; IN uses a nonempty value list or SELECT.
                               LIKE uses a string literal, preserving its original token text.
        selector             : Plain column or COUNT/AVG/MAX/MIN/SUM applied to a column.
                               aggregate=none requires a column; COUNT(*) has no column.
        column_declaration   : Name, type, optional VARCHAR length, nullability and DEFAULT token.
        key_declaration      : Index, primary key, foreign key or UNIQUE, with ordered column names.
                               A foreign-key definition also owns a referenced table/column list.
        create_table_statement: Ordered column and key declarations; SQL.g4 permits PRIMARY KEY
                               and FOREIGN KEY table constraints here, not INDEX or UNIQUE.
        insert_statement     : Target table and VALUES rows in input order.
        select_statement     : Nonempty source table list, projection, WHERE, GROUP BY, ORDER BY
                               and LIMIT/OFFSET. Preserve table, selector and predicate order.
                               SELECT * has select_all=true and no explicit selectors.
                               Otherwise selectors must be nonempty.
        order_clause         : One ordering column; absent direction and ASC both mean ascending.
        limit_clause         : Integer count token and optional integer offset token.
                               OFFSET occurs only with LIMIT; retain tokens until range validation.
        update_statement     : Target table, ordered assignments and nonempty WHERE conditions.
        delete_statement     : Target table and conditions; an empty list means all rows.
        load_statement       : Target table, input path and field delimiter as string literals.
        alter_table_statement: Target table, ADD/DROP operation and the affected key declaration.
                               ADD supports INDEX, PRIMARY KEY, FOREIGN KEY and UNIQUE.
                               DROP supports INDEX, PRIMARY KEY and FOREIGN KEY.
                               DROP has no columns or reference; INDEX/FOREIGN KEY require a name,
                               while PRIMARY KEY allows an omitted name, following SQL.g4.

    ownership:
        AST strings and vectors own their data; never retain token or parse-tree pointers.
        Recursive SELECT operands use unique_ptr<select_statement> and must be non-null.
        This makes predicates, queries and the statement variant move-only; transfer ownership
        when constructing or returning them instead of copying parser-owned objects.
        All successful parse results remain valid after lexer/parser/input destruction.

    grammar:
        db_statement      -> command_statement.
        table_statement   -> command/create_table/load/insert/delete/update/select statements.
        alter_statement   -> alter_table_statement using a shared key_declaration.
        field             -> column_declaration or key_declaration in the fields vector.
        where_clause      -> predicate, including nested SELECTs and value lists.
        selectors         -> select_all or ordered selectors, including aggregates.
        Annotation/NULL   -> command_statement with type=noop and no name.
        Reuse structures for matching data shapes; do not create one wrapper per keyword.
        Every grammar branch has a structured representation; no raw-SQL fallback is needed.

    methods:
        parse_one : Borrow sql only until return; permit surrounding whitespace.
                    Require exactly one complete SQL statement with its terminating semicolon.
                    Run the SQL.g4 lexer/parser and inspect errors from both.
                    Convert only an error-free parse tree into a statement.
                    Convert all clauses and nested queries without discarding any operation.
                    Parsing does not classify statements by the execution layer's capabilities.

    helpers:
        Keep generated ANTLR headers, error listeners and conversion helpers inside this cpp.
        Disable ANTLR diagnostic printing; report errors through db_error/error_context.
        Follow the checked-in grammar, including uppercase keywords, all-column INSERT and
        mandatory WHERE for UPDATE. Do not extend or reduce the accepted dialect here.
        Validate the structural combinations described above while converting the parse tree.
        Object lookup, type compatibility and constraint semantics belong to binding/execution.

    errors:
        Empty input, missing terminator, multiple statements or lexical/syntax errors
        -> syntax_error with one-based line/column in error_context.
        A recovered parse tree must not be converted if any error was reported.
        Allocation failures propagate; failed parsing publishes no partial AST.
        Destroying the input after success does not invalidate the returned statement.
        No catalog, table or session state is modified.
*/

}
