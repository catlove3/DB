#include <db/record/record.h>
#include <db/storage/file_manager.h>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <utility>

using namespace db;

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

// 被拒绝的操作必须保留整张输入页。
template<class F>
void expect_unchanged(page & p,error_code expected,F&& f)
{
    const page before = p;
    expect_error(expected,f);
    CHECK(p == before);
}

record_bytes payload(slot_id size,unsigned seed)
{
    record_bytes bytes(size);
    for(std::size_t i = 0;i < bytes.size();++i)
        bytes[i] = static_cast<std::byte>((seed + i * 17) & 0xff);
    return bytes;
}

void test_capacity_and_init()
{
    const slot_id sizes[] = {1,8,31,32,51,64,2048,8127};
    const slot_id slots[] = {7224,1000,261,253,158,126,3,1};
    for(std::size_t i = 0;i < std::size(sizes);++i)
    {
        CHECK(record_page_helper::capacity(sizes[i]) == slots[i]);
        page p;
        p.fill(std::byte{0xff});
        record_page_helper::init(p,sizes[i]);
        record_page_helper::validate(p,sizes[i]);
        CHECK(read_u32(p.data()) == 0x31504d52);
        CHECK(read_u32(p.data()+4) == 1);
        CHECK(read_u32(p.data()+8) == sizes[i]);
        CHECK(read_u32(p.data()+12) == slots[i]);
        CHECK(read_u32(p.data()+16) == 0);
        CHECK(read_u32(p.data()+20) == 0);
        CHECK(read_u64(p.data()+24) == invalid_page_id);
        CHECK(std::all_of(p.begin()+32,p.end(),[](std::byte b){return b == std::byte{0};}));
        CHECK(record_page_helper::record_count(p) == 0);
        CHECK(record_page_helper::first_free(p) == 0);
    }

    page p{};
    record_page_helper::init(p,32);
    for(slot_id size : {0u,8128u,std::numeric_limits<slot_id>::max()})
    {
        expect_error(error_code::invalid_argument,[&]{record_page_helper::capacity(size);});
        expect_unchanged(p,error_code::invalid_argument,[&]{record_page_helper::init(p,size);});
        expect_unchanged(p,error_code::invalid_argument,[&]{record_page_helper::validate(p,size);});
    }
    record_page_helper::clear(p);
    CHECK(std::all_of(p.begin(),p.end(),[](std::byte b){return b == std::byte{0};}));
    expect_error(error_code::invalid_format,[&]{record_page_helper::record_count(p);});
}

void test_fill_and_reuse()
{
    for(slot_id size : {1u,8u,31u,32u,51u,64u,2048u,8127u})
    {
        page p{};
        record_page_helper::init(p,size);
        const slot_id slots = record_page_helper::capacity(size);
        std::vector<record_bytes> expected;
        for(slot_id i = 0;i < slots;++i)
        {
            CHECK(record_page_helper::first_free(p) == i);
            CHECK(!record_page_helper::occupied(p,i));
            expected.push_back(payload(size,i));
            CHECK(record_page_helper::insert(p,expected.back()) == i);
            CHECK(record_page_helper::occupied(p,i));
        }
        CHECK(record_page_helper::record_count(p) == slots);
        CHECK(!record_page_helper::first_free(p));
        const page full = p;
        CHECK(!record_page_helper::insert(p,expected[0]));
        CHECK(p == full);
        for(slot_id i = 0;i < slots;++i)
            CHECK(record_page_helper::read(p,i) == expected[i]);

        // 字节 7/8、15/16 边界，以及最后一个有效槽。
        const slot_id candidates[] = {0,7,8,15,16,slots-1};
        std::vector<slot_id> holes;
        for(slot_id slot : candidates)
            if(slot < slots && std::find(holes.begin(),holes.end(),slot) == holes.end())
                holes.push_back(slot);
        std::sort(holes.begin(),holes.end());
        for(slot_id slot : holes)
        {
            record_page_helper::erase(p,slot);
            CHECK(!record_page_helper::occupied(p,slot));
            expect_unchanged(p,error_code::record_not_found,[&]{record_page_helper::read(p,slot);});
            expect_unchanged(p,error_code::record_not_found,[&]{record_page_helper::erase(p,slot);});
        }
        CHECK(record_page_helper::record_count(p) == slots - holes.size());
        for(slot_id slot : holes)
        {
            expected[slot] = payload(size,slot+31);
            CHECK(record_page_helper::insert(p,expected[slot]) == slot);
        }
        for(slot_id i = 0;i < slots;++i)
            CHECK(record_page_helper::read(p,i) == expected[i]);
    }
}

void test_layout_and_rejections()
{
    page p{};
    record_page_helper::init(p,32);
    // 文件链信息由文件层维护，单页 CRUD 应保留这些字段。
    write_u32(p.data()+20,1);
    write_u64(p.data()+24,42);
    const record_bytes original = payload(32,11);
    CHECK(record_page_helper::insert(p,original) == 0);
    CHECK(p[64] == std::byte{1});
    CHECK(std::equal(original.begin(),original.end(),p.begin()+96));

    auto copy = record_page_helper::read(p,0);
    copy[0] ^= std::byte{0xff};
    CHECK(record_page_helper::read(p,0) == original);

    for(slot_id size : {0u,31u,33u})
    {
        const record_bytes wrong(size);
        expect_unchanged(p,error_code::invalid_argument,[&]{record_page_helper::insert(p,wrong);});
        expect_unchanged(p,error_code::invalid_argument,[&]{record_page_helper::update(p,0,wrong);});
        expect_unchanged(p,error_code::invalid_argument,[&]{record_page_helper::update(p,invalid_slot_id,wrong);});
    }
    for(slot_id slot : {253u,invalid_slot_id})
    {
        expect_unchanged(p,error_code::invalid_rid,[&]{record_page_helper::occupied(p,slot);});
        expect_unchanged(p,error_code::invalid_rid,[&]{record_page_helper::read(p,slot);});
        expect_unchanged(p,error_code::invalid_rid,[&]{record_page_helper::update(p,slot,original);});
        expect_unchanged(p,error_code::invalid_rid,[&]{record_page_helper::erase(p,slot);});
    }
    expect_unchanged(p,error_code::record_not_found,[&]{record_page_helper::update(p,1,original);});

    const record_bytes changed = payload(32,99);
    record_page_helper::update(p,0,changed);
    CHECK(record_page_helper::read(p,0) == changed);
    CHECK(record_page_helper::record_count(p) == 1);
    CHECK(p[64] == std::byte{1});
    record_page_helper::erase(p,0);
    CHECK(std::equal(changed.begin(),changed.end(),p.begin()+96));
    CHECK(!record_page_helper::occupied(p,0));
    CHECK(record_page_helper::record_count(p) == 0);
    expect_unchanged(p,error_code::record_not_found,[&]{record_page_helper::read(p,0);});
    CHECK(record_page_helper::insert(p,original) == 0);
    CHECK(record_page_helper::read(p,0) == original);
    CHECK(read_u32(p.data()+20) == 1);
    CHECK(read_u64(p.data()+24) == 42);

    // 页满也必须先拒绝错误长度。
    record_page_helper::init(p,8127);
    CHECK(record_page_helper::insert(p,payload(8127,1)) == 0);
    expect_unchanged(p,error_code::invalid_argument,[&]{record_page_helper::insert(p,original);});
}

void test_corruption()
{
    page valid{};
    record_page_helper::init(valid,32);
    const std::pair<std::size_t,std::uint32_t> corruptions[] = {
        {0,0},{0,db::magic},{4,2},{8,0},{8,31},{8,8128},{8,invalid_slot_id},
        {12,0},{12,invalid_slot_id},{16,254},{16,1},{20,2},{32,1},{60,1}
    };
    std::vector<page> bad_pages;
    for(const auto & change : corruptions)
    {
        page p = valid;
        write_u32(p.data()+change.first,change.second);
        bad_pages.push_back(p);
    }
    page bad_tail = valid;
    bad_tail[95] = std::byte{0x80};
    bad_pages.push_back(bad_tail);

    for(page & p : bad_pages)
    {
        const record_bytes bytes(32);
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::validate(p,32);});
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::record_count(p);});
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::occupied(p,0);});
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::first_free(p);});
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::read(p,0);});
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::insert(p,bytes);});
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::update(p,0,bytes);});
        expect_unchanged(p,error_code::invalid_format,[&]{record_page_helper::erase(p,0);});
    }
}

void test_random_operations()
{
    page p{};
    record_page_helper::init(p,64);
    const slot_id slots = record_page_helper::capacity(64);
    std::vector<std::optional<record_bytes>> model(slots);
    std::mt19937 rng(20261105);
    for(unsigned step = 0;step < 1000;++step)
    {
        const slot_id slot = rng() % slots;
        const unsigned operation = rng() % 3;
        if(operation == 0)
        {
            const record_bytes bytes = payload(64,rng());
            const auto free = std::find(model.begin(),model.end(),std::nullopt);
            const auto inserted = record_page_helper::insert(p,bytes);
            if(free == model.end())
                CHECK(!inserted);
            else
            {
                const slot_id expected = free-model.begin();
                CHECK(inserted == expected);
                model[expected] = bytes;
            }
        }
        else if(model[slot])
        {
            if(operation == 1)
            {
                const record_bytes bytes = payload(64,rng());
                record_page_helper::update(p,slot,bytes);
                model[slot] = bytes;
            }
            else
            {
                record_page_helper::erase(p,slot);
                model[slot].reset();
            }
        }
        else
            expect_unchanged(p,error_code::record_not_found,[&]{record_page_helper::erase(p,slot);});

        slot_id count = 0;
        for(slot_id i = 0;i < slots;++i)
        {
            CHECK(record_page_helper::occupied(p,i) == model[i].has_value());
            if(model[i])
            {
                ++count;
                CHECK(record_page_helper::read(p,i) == *model[i]);
            }
        }
        CHECK(record_page_helper::record_count(p) == count);
    }
}

}

int main()
{
    try
    {
        test_capacity_and_init();
        test_fill_and_reuse();
        test_layout_and_rejections();
        test_corruption();
        test_random_operations();
    }
    catch(const std::exception & error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if(failures != 0)
        return 1;
    std::cout << "record page tests passed\n";
}
