#include <db/parser/parser.h>
#include <db/common/error.h>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <utility>
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

template<class F>
void expect_error(db::error_code code, F&& action)
{
    try
    {
        action();
        CHECK(false);
    }
    catch(const db::db_error& error)
    {
        CHECK(error.code() == code);
        CHECK(error.context().operation == "read_statement");
    }
}

struct reader_case
{
    std::string input;
    std::vector<std::string> statements;
};

void test_inputs(std::ios::iostate mask)
{
    const std::vector<reader_case> cases{
        {"", {}},
        {" \t\r\n\n", {}},
        {"SHOW TABLES;", {"SHOW TABLES;"}},
        {"SHOW TABLES;  \n", {"SHOW TABLES;"}},
        {"SHOW TABLES; SHOW DATABASES;\n", {"SHOW TABLES;", " SHOW DATABASES;"}},
        {"SELECT\nname\nFROM student;", {"SELECT\nname\nFROM student;"}},
        {"INSERT INTO t VALUES ('a;b');", {"INSERT INTO t VALUES ('a;b');"}},
        {"INSERT INTO t VALUES ('a\n\n  \nb');\n", {"INSERT INTO t VALUES ('a\n\n  \nb');"}},
        {"INSERT INTO t VALUES ('甲\n乙');", {"INSERT INTO t VALUES ('甲\n乙');"}},
        {"SELECT *\r\nFROM t;", {"SELECT *\r\nFROM t;"}},
        {"-\n-;", {"-\n-;"}}, // Two separated minus signs do not start a comment.
        {"-- note;", {}},
        {"---;", {}},
        {"--\n;", {}}, // The newline satisfies Annotation's required content.
        {" \t-- comment;\n", {}},
        {"-- first\nsecond;SELECT * FROM t;", {"SELECT * FROM t;"}},
        {"-- first; -- second;NULL;", {"NULL;"}},
        {"SHOW TABLES; -- comment;\n", {"SHOW TABLES;"}},
        {"  -- comment;\nSHOW TABLES;", {"\nSHOW TABLES;"}},
        {"INSERT INTO t VALUES ('-- text;');", {"INSERT INTO t VALUES ('-- text;');"}},
        {"exit", {}},
        {"  exit \t\r\n", {}},
        {"SHOW TABLES; \n \texit \r\nSHOW DATABASES;", {"SHOW TABLES;"}},
        {"SELECT\nexit\nFROM t;", {"SELECT\nexit\nFROM t;"}},
        {"INSERT INTO t VALUES ('a\nexit\nb');", {"INSERT INTO t VALUES ('a\nexit\nb');"}},
        {"-- note\nexit\n;SHOW TABLES;", {"SHOW TABLES;"}},
        {"exit123;", {"exit123;"}}, // Reader splits text; parser checks SQL syntax.
        {"exit;", {"exit;"}},
        {"EXIT;", {"EXIT;"}},
        {"SHOW TABLES;exit;", {"SHOW TABLES;", "exit;"}},
    };

    for(const auto& test : cases)
    {
        std::istringstream input(test.input);
        input.exceptions(mask);
        db::statement_reader reader(input);
        for(const auto& expected : test.statements)
        {
            const auto result = reader.next();
            CHECK(result.has_value());
            if(result)
                CHECK(*result == expected);
        }
        CHECK(!reader.next());
        CHECK(!reader.next());
        CHECK(input.exceptions() == mask);
    }

    for(const char* text : {"x", "SELECT *", "'unclosed", "--", "-- note\n", "--;",
                           "SHOW -- comment;TABLES;", "SHOW TABLES; exit\n"})
    {
        std::istringstream input(text);
        input.exceptions(mask);
        db::statement_reader reader(input);
        expect_error(db::error_code::syntax_error, [&] {
            while(reader.next())
            {
            }
        });
        expect_error(db::error_code::syntax_error, [&] { (void)reader.next(); });
        CHECK(input.exceptions() == mask);
    }
}

// Supply a prefix, then fail during an actual stream read (not just a preset flag).
class failing_buffer : public std::streambuf
{
    std::string prefix_;
    std::size_t offset_ = 0;
    char current_ = 0;
    bool allocation_failure_;

    int_type underflow() override
    {
        ++reads;
        if(offset_ == prefix_.size())
        {
            if(allocation_failure_)
                throw std::bad_alloc();
            throw std::runtime_error("injected stream failure");
        }
        current_ = prefix_[offset_++];
        setg(&current_, &current_, &current_ + 1);
        return traits_type::to_int_type(current_);
    }

public:
    std::size_t reads = 0;

    explicit failing_buffer(std::string prefix, bool allocation_failure = false)
        : prefix_(std::move(prefix)), allocation_failure_(allocation_failure)
    {
    }
};

void test_read_failures(std::ios::iostate mask)
{
    for(const char* prefix : {"", "SHOW TABLES;", "SHOW TABLES;\nSELECT"})
    {
        failing_buffer buffer(prefix);
        std::istream input(&buffer);
        input.exceptions(mask);
        db::statement_reader reader(input);
        CHECK(buffer.reads == 0); // Construction must not read.
        if(std::string_view(prefix) == "SHOW TABLES;\nSELECT")
            CHECK(reader.next() == std::optional<std::string>{"SHOW TABLES;"});
        expect_error(db::error_code::io_error, [&] { (void)reader.next(); });
        const auto reads = buffer.reads;
        expect_error(db::error_code::syntax_error, [&] { (void)reader.next(); });
        CHECK(buffer.reads == reads);
        CHECK(input.exceptions() == mask);
    }
}

void test_terminal_states()
{
    for(auto state : {std::ios::badbit, std::ios::failbit, std::ios::badbit | std::ios::eofbit})
    {
        std::istringstream input;
        input.setstate(state);
        db::statement_reader reader(input);
        expect_error(db::error_code::io_error, [&] { (void)reader.next(); });
    }

    std::istringstream input("exit\nSHOW TABLES;\n");
    db::statement_reader reader(input);
    CHECK(!reader.next());
    CHECK(!reader.next());
    std::string remainder;
    CHECK(static_cast<bool>(std::getline(input, remainder)));
    CHECK(remainder == "SHOW TABLES;"); // Exit never consumes subsequent lines.

    failing_buffer buffer("", true);
    std::istream failing_input(&buffer);
    failing_input.exceptions(std::ios::badbit);
    db::statement_reader failing_reader(failing_input);
    try
    {
        (void)failing_reader.next();
        CHECK(false);
    }
    catch(const std::bad_alloc&)
    {
    }
    expect_error(db::error_code::syntax_error, [&] { (void)failing_reader.next(); });
}
} // namespace

int main()
{
    try
    {
        for(unsigned bits = 0; bits < 8; ++bits)
        {
            auto mask = std::ios::goodbit;
            if(bits & 1) mask |= std::ios::badbit;
            if(bits & 2) mask |= std::ios::failbit;
            if(bits & 4) mask |= std::ios::eofbit;
            test_inputs(mask);
            test_read_failures(mask);
        }
        test_terminal_states();
    }
    catch(const std::exception& error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if(failures)
        return 1;
    std::cout << "statement reader checks passed\n";
}
