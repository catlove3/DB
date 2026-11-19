#include <SQLLexer.h>
#include <SQLParser.h>
#include <SQLBaseVisitor.h>
#include <antlr4-runtime.h>
#include <Version.h>
#include <any>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(condition) \
    do { \
        if(!(condition)) { \
            std::cerr << __FILE__ << ':' << __LINE__ \
                      << ": CHECK failed: " #condition << '\n'; \
            ++failures; \
        } \
    } while(false)

struct diagnostic
{
    std::size_t line;
    std::size_t column;
};

class error_listener final : public antlr4::BaseErrorListener
{
public:
    std::vector<diagnostic> errors;

    void syntaxError(antlr4::Recognizer*, antlr4::Token*, std::size_t line,
                     std::size_t column, const std::string&, std::exception_ptr) override
    {
        errors.push_back({line, column + 1});
    }
};

class command_visitor final : public SQLBaseVisitor
{
public:
    unsigned show_tables = 0;
    std::string database;

    std::any visitShow_tables(SQLParser::Show_tablesContext*) override
    {
        ++show_tables;
        return {};
    }

    std::any visitUse_db(SQLParser::Use_dbContext* node) override
    {
        database = node->Identifier()->getText();
        return {};
    }
};

struct parse_result
{
    std::size_t statements = 0;
    std::vector<diagnostic> lexical_errors;
    std::vector<diagnostic> syntax_errors;
    unsigned show_tables = 0;
    std::string database;
};

parse_result parse(const std::string& sql)
{
    // Exercise generated C++ code and the real runtime, without implementing db::parser.
    antlr4::ANTLRInputStream input(sql);
    error_listener lexical_errors;
    SQLLexer lexer(&input);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&lexical_errors);
    antlr4::CommonTokenStream tokens(&lexer);
    tokens.fill();

    error_listener syntax_errors;
    SQLParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&syntax_errors);
    auto* tree = parser.program();
    CHECK(tree != nullptr);

    command_visitor visitor;
    if(lexical_errors.errors.empty() && syntax_errors.errors.empty() && tree)
        visitor.visit(tree);
    return {tree ? tree->statement().size() : 0, lexical_errors.errors,
            syntax_errors.errors, visitor.show_tables, visitor.database};
}
} // namespace

int main()
{
    static_assert(ANTLRCPP_VERSION == 413002);
    CHECK(antlr4::RuntimeMetaData::getRuntimeVersion() == "4.13.2");
    for(const char* sql : {
        "SHOW TABLES;", "SHOW DATABASES;", "SHOW INDEXES;", "USE school;",
        "CREATE DATABASE school;", "DROP DATABASE school;", "DROP TABLE t;", "DESC t;",
        "CREATE TABLE t (id INT NOT NULL DEFAULT 0, name VARCHAR(32), score FLOAT, PRIMARY KEY (id));",
        "CREATE TABLE t (id INT, FOREIGN KEY (id) REFERENCES parent (id));",
        "INSERT INTO t VALUES (1, 'a;b'), (2, NULL);",
        "SELECT COUNT(*), AVG(t.score) FROM t WHERE t.id IN (SELECT id FROM u) GROUP BY t.id ORDER BY t.id DESC LIMIT 10 OFFSET 2;",
        "SELECT * FROM t WHERE id >= 1 AND name LIKE 'x%' AND score IS NOT NULL;",
        "UPDATE t SET id = 2 WHERE id = 1;", "DELETE FROM t;",
        "LOAD DATA INFILE 'data.csv' INTO TABLE t FIELDS TERMINATED BY ',';",
        "ALTER TABLE t ADD INDEX idx (id);", "ALTER TABLE t DROP INDEX idx;",
        "ALTER TABLE t ADD CONSTRAINT pk PRIMARY KEY (id);", "ALTER TABLE t DROP PRIMARY KEY;",
        "ALTER TABLE t ADD FOREIGN KEY (id) REFERENCES u (id);", "ALTER TABLE t DROP FOREIGN KEY fk;",
        "ALTER TABLE t ADD UNIQUE uq (id);", "NULL;", "-- comment;", "---;", "--\n;"
    })
    {
        const auto result = parse(sql);
        CHECK(result.lexical_errors.empty());
        CHECK(result.syntax_errors.empty());
        CHECK(result.statements == 1);
    }

    const auto commands = parse("SHOW TABLES; USE school;");
    CHECK(commands.statements == 2); // program permits multiple statements; parse_one will restrict it.
    CHECK(commands.show_tables == 1);
    CHECK(commands.database == "school");
    const auto empty = parse("");
    CHECK(empty.statements == 0 && empty.syntax_errors.empty());

    for(const char* sql : {"SHOW TABLES", "show tables;", "SELECT AVG(*) FROM t;",
                           "UPDATE t SET id = 1;", "--;"})
    {
        const auto result = parse(sql);
        CHECK(!result.lexical_errors.empty() || !result.syntax_errors.empty());
    }

    const auto lexical = parse("SHOW TABLES; @");
    CHECK(!lexical.lexical_errors.empty());
    CHECK(lexical.show_tables == 0); // Do not visit a tree after a lexical error.
    if(!lexical.lexical_errors.empty())
        CHECK(lexical.lexical_errors.front().line == 1 && lexical.lexical_errors.front().column == 14);

    const auto syntax = parse("SELECT *\nFROM ;");
    CHECK(syntax.lexical_errors.empty());
    CHECK(!syntax.syntax_errors.empty());
    if(!syntax.syntax_errors.empty())
        CHECK(syntax.syntax_errors.front().line == 2 && syntax.syntax_errors.front().column == 6);

    if(failures)
        return 1;
    std::cout << "generated SQL grammar checks passed\n";
}
