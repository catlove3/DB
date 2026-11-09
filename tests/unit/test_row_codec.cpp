#include <db/record/record.h>
#include <db/storage/buffer_pool.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <utility>
#include <unistd.h>

using namespace db;
namespace fs = std::filesystem;

namespace{

int failures = 0;

#define CHECK(condition) \
    do { \
        if(!(condition)) { \
            std::cerr << __FILE__ << ":" << __LINE__ \
                      << ": CHECK failed: " #condition << '\n'; \
            ++failures; \
        } \
    } while(false)

template<class F>
void expect_error(error_code expected,F&& f)
{
    try
    {
        f();
        CHECK(false);
    }
    catch(const db_error & error)
    {
        CHECK(error.code() == expected);
    }
}

void expect_bad(const row_codec & codec,const record_bytes & bytes)
{
    const record_bytes before = bytes;
    expect_error(error_code::invalid_format,[&]{codec.decode(bytes);});
    CHECK(bytes == before);
}

void test_schema()
{
    expect_error(error_code::invalid_argument,[]{row_codec codec({});});
    for(data_type type : {data_type::INT,data_type::FLOAT})
        expect_error(error_code::invalid_argument,[&]{row_codec codec({{type,1,true}});});
    for(uint32_t length : {0u,65536u})
        expect_error(error_code::invalid_argument,[&]{row_codec codec({{data_type::VARCHAR,length,true}});});
    expect_error(error_code::invalid_argument,[]{row_codec codec({{static_cast<data_type>(99),0,true}});});
    for(uint32_t length : {8125u,65535u})
        expect_error(error_code::value_out_of_range,[&]{row_codec codec({{data_type::VARCHAR,length,true}});});
    expect_error(error_code::value_out_of_range,[]{
        row_codec codec(std::vector<column_spec>(65536,{data_type::INT,0,true}));
    });
    row_codec largest({{data_type::VARCHAR,8124,true}});
    CHECK(largest.record_size() == 8127);
    const row input{std::string(8124,'x')};
    CHECK(largest.decode(largest.encode(input)) == input);
}

void test_exact_bytes()
{
    row_codec codec({{data_type::INT,0,true},{data_type::VARCHAR,3,false}});
    const row input{std::monostate{},std::string{"ab"}};
    const unsigned raw[] = {0x01,0,0,0,0,0x02,0,0x61,0x62,0};
    record_bytes expected;
    for(unsigned byte : raw)
        expected.push_back(static_cast<std::byte>(byte));
    CHECK(codec.record_size() == 10);
    CHECK(codec.encode(input) == expected);
    CHECK(codec.decode(expected) == input);

    row_codec triple({
        {data_type::INT,0,false},
        {data_type::VARCHAR,16,true},
        {data_type::FLOAT,0,true}
    });
    const row values{std::int32_t{42},std::string{"Ann"},3.5};
    record_bytes bytes(31,std::byte{0});
    bytes[1] = std::byte{0x2a};
    bytes[5] = std::byte{3};
    bytes[7] = std::byte{0x41};
    bytes[8] = std::byte{0x6e};
    bytes[9] = std::byte{0x6e};
    bytes[29] = std::byte{0x0c};
    bytes[30] = std::byte{0x40};
    CHECK(triple.record_size() == 31);
    CHECK(triple.encode(values) == bytes);
    CHECK(triple.decode(bytes) == values);
    const row before = values;
    triple.encode(values);
    CHECK(values == before);
}

void test_numbers()
{
    row_codec integers({{data_type::INT,0,true}});
    for(std::int32_t number : {
        std::numeric_limits<std::int32_t>::min(),std::int32_t{-1},std::int32_t{0},
        std::int32_t{1},std::numeric_limits<std::int32_t>::max()})
    {
        const row input{number};
        CHECK(integers.decode(integers.encode(input)) == input);
    }
    record_bytes minimum(5,std::byte{0});
    minimum[4] = std::byte{0x80};
    CHECK(integers.encode(row{std::numeric_limits<std::int32_t>::min()}) == minimum);
    CHECK(std::get<std::int32_t>(integers.decode(minimum)[0]) == std::numeric_limits<std::int32_t>::min());

    row_codec floats({{data_type::FLOAT,0,true}});
    for(double number : {3.5,-42.125,0.0,-0.0,
        std::numeric_limits<double>::max(),std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min()})
    {
        const row result = floats.decode(floats.encode(row{number}));
        const double restored = std::get<double>(result[0]);
        CHECK(restored == number);
        CHECK(std::signbit(restored) == std::signbit(number));
    }
    record_bytes negative_zero(9,std::byte{0});
    negative_zero[8] = std::byte{0x80};
    CHECK(floats.encode(row{-0.0}) == negative_zero);
    for(double number : {std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        expect_error(error_code::value_out_of_range,[&]{floats.encode(row{number});});
    for(uint64_t bits : {UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff8000000000000)})
    {
        record_bytes bad(9,std::byte{0});
        write_u64(bad.data()+1,bits);
        expect_bad(floats,bad);
    }
}

void test_strings_and_nulls()
{
    row_codec strings({{data_type::VARCHAR,6,true}});
    for(const std::string & text : {std::string{},std::string{"abcdef"},
        std::string{"a\0b",3},std::string{u8"你好"}})
    {
        const row input{text};
        CHECK(strings.decode(strings.encode(input)) == input);
    }
    const row null{std::monostate{}};
    const auto encoded_null = strings.encode(null);
    const auto encoded_empty = strings.encode(row{std::string{}});
    CHECK(encoded_null != encoded_empty);
    CHECK(encoded_null[0] == std::byte{1});
    CHECK(encoded_empty[0] == std::byte{0});
    CHECK(std::holds_alternative<std::monostate>(strings.decode(encoded_null)[0]));
    CHECK(std::holds_alternative<std::string>(strings.decode(encoded_empty)[0]));
    row_codec small({{data_type::VARCHAR,3,true}});
    expect_error(error_code::value_out_of_range,[&]{small.encode(row{std::string{"abcd"}});});
    expect_error(error_code::value_out_of_range,[&]{small.encode(row{std::string{u8"你好"}});});

    row_codec bitmap(std::vector<column_spec>(10,{data_type::INT,0,true}));
    row input(10,std::int32_t{7});
    for(std::size_t i : {0u,7u,8u,9u})
        input[i] = std::monostate{};
    const auto encoded = bitmap.encode(input);
    CHECK(bitmap.record_size() == 42);
    CHECK(encoded[0] == std::byte{0x81});
    CHECK(encoded[1] == std::byte{3});
    CHECK(bitmap.decode(encoded) == input);
    record_bytes bad = encoded;
    bad[1] |= std::byte{4};
    expect_bad(bitmap,bad);
    row_codec eight(std::vector<column_spec>(8,{data_type::INT,0,true}));
    const row all_null(8,std::monostate{});
    const auto full_bitmap = eight.encode(all_null);
    CHECK(full_bitmap[0] == std::byte{0xff});
    CHECK(eight.decode(full_bitmap) == all_null);
}

void test_invalid_values_and_bytes()
{
    row_codec codec({
        {data_type::INT,0,false},{data_type::VARCHAR,3,true},{data_type::FLOAT,0,true}
    });
    const row valid{std::int32_t{7},std::string{"a"},3.5};
    expect_error(error_code::invalid_argument,[&]{codec.encode(row{});});
    expect_error(error_code::invalid_argument,[&]{codec.encode(row(4));});
    for(std::size_t i = 0;i < 3;++i)
    {
        row wrong = valid;
        wrong[i] = i == 0 ? value{std::string{"7"}} : value{std::int32_t{7}};
        expect_error(error_code::type_mismatch,[&]{codec.encode(wrong);});
    }
    row illegal_null = valid;
    illegal_null[0] = std::monostate{};
    expect_error(error_code::not_null_violation,[&]{codec.encode(illegal_null);});
    const record_bytes bytes = codec.encode(valid);
    expect_bad(codec,record_bytes{});
    record_bytes short_row = bytes;
    short_row.pop_back();
    expect_bad(codec,short_row);
    record_bytes long_row = bytes;
    long_row.push_back(std::byte{0});
    expect_bad(codec,long_row);
    record_bytes bad = bytes;
    bad[0] |= std::byte{1};
    expect_bad(codec,bad);
    bad = bytes;
    bad[0] |= std::byte{0x80};
    expect_bad(codec,bad);
    for(unsigned length : {4u,65535u})
    {
        bad = bytes;
        bad[5] = static_cast<std::byte>(length & 0xff);
        bad[6] = static_cast<std::byte>((length >> 8) & 0xff);
        expect_bad(codec,bad);
    }
    bad = bytes;
    bad[8] = std::byte{1}; // Nonzero padding after the one-byte string.
    expect_bad(codec,bad);

    row_codec nullable({
        {data_type::INT,0,true},{data_type::VARCHAR,3,true},{data_type::FLOAT,0,true}
    });
    const record_bytes null_bytes = nullable.encode(row(3,std::monostate{}));
    for(std::size_t offset : {1u,5u,7u,10u,17u})
    {
        bad = null_bytes;
        bad[offset] = std::byte{1};
        expect_bad(nullable,bad);
    }
}

struct temp_dir
{
    fs::path path;
    temp_dir()
    {
        path = fs::temp_directory_path() / ("idbs_row_codec_" + std::to_string(getpid()));
        fs::create_directory(path);
    }
    ~temp_dir()
    {
        std::error_code ec;
        fs::remove_all(path,ec);
    }
};

void test_record_file_integration()
{
    temp_dir tmp;
    file_manager fm;
    const fs::path path = tmp.path/"rows.db";
    fm.create_file(path.string());
    file_id fid = fm.open_file(path.string());
    buffer_pool_manager bpm(fm,2);
    row_codec codec({
        {data_type::INT,0,false},{data_type::VARCHAR,16,true},{data_type::FLOAT,0,true}
    });
    record_file::initialize(fm,bpm,fid,codec.record_size());
    const slot_id slots = record_page_helper::capacity(codec.record_size());
    std::vector<row> rows;
    std::vector<rid> ids;
    {
        record_file file(fm,bpm,fid);
        for(slot_id i = 0;i < slots * 2 + 3;++i)
        {
            rows.push_back(row{static_cast<std::int32_t>(i),std::string{"row"}+std::to_string(i),double(i)+0.25});
            ids.push_back(file.insert(codec.encode(rows.back())));
        }
        file.erase(ids[0]);
        for(slot_id i = slots;i < slots * 2;++i)
            file.erase(ids[i]); // Leave an entire empty page between live pages.
        rows[1][1] = std::string{};
        rows[1][2] = std::monostate{};
        file.update(ids[1],codec.encode(rows[1]));
        CHECK(file.record_count() == slots + 2);
    }
    bpm.close_file(fid);
    fid = fm.open_file(path.string());
    {
        record_file file(fm,bpm,fid);
        auto cursor = file.scan();
        slot_id expected = 1;
        while(auto item = cursor.next())
        {
            CHECK(expected < rows.size());
            if(expected >= rows.size())
                break;
            CHECK(item->id == ids[expected]);
            CHECK(codec.decode(item->bytes) == rows[expected]);
            ++expected;
            if(expected == slots)
                expected = slots * 2;
            bpm.check_invariants();
        }
        CHECK(expected == rows.size());
        CHECK(!cursor.next());
        CHECK(file.record_count() == slots + 2);
        CHECK(fm.get_file(fid).get_num_pages() == 5);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

}

int main()
{
    try
    {
        test_schema();
        test_exact_bytes();
        test_numbers();
        test_strings_and_nulls();
        test_invalid_values_and_bytes();
        test_record_file_integration();
    }
    catch(const std::exception & error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if(failures != 0)
        return 1;
    std::cout << "row codec tests passed\n";
}
