#include <db/record/record.h>
#include <db/storage/buffer_pool.h>
#include <algorithm>
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

struct temp_dir
{
    fs::path path;
    temp_dir()
    {
        path = fs::temp_directory_path() / ("idbs_record_file_" + std::to_string(getpid()));
        fs::create_directory(path);
    }
    ~temp_dir()
    {
        std::error_code ec;
        fs::remove_all(path,ec);
    }
};

file_id create_file(file_manager & fm,const fs::path & path)
{
    fm.create_file(path.string());
    return fm.open_file(path.string());
}

page read_page(buffer_pool_manager & bpm,file_id fid,page_id pid)
{
    auto guard = bpm.fetch_page(fid,pid);
    return *guard.view();
}

void write_page(buffer_pool_manager & bpm,file_id fid,page_id pid,const page & p)
{
    auto guard = bpm.fetch_page(fid,pid,access_type::write);
    *guard.get() = p;
}

void test_initialize(temp_dir & tmp)
{
    file_manager fm;
    const file_id fid = create_file(fm,tmp.path/"init.db");
    buffer_pool_manager small(fm,1);
    expect_error(error_code::invalid_argument,[&]{record_file::initialize(fm,small,fid,32);});
    CHECK(fm.get_file(fid).get_num_pages() == 1);
    expect_error(error_code::invalid_argument,[&]{record_file file(fm,small,fid);});

    buffer_pool_manager bpm(fm,2);
    expect_error(error_code::invalid_format,[&]{record_file file(fm,bpm,fid);});
    for(slot_id size : {0u,8128u})
        expect_error(error_code::invalid_argument,[&]{record_file::initialize(fm,bpm,fid,size);});
    CHECK(fm.get_file(fid).get_num_pages() == 1);

    record_file::initialize(fm,bpm,fid,32);
    const page meta = read_page(bpm,fid,1);
    CHECK(read_u32(meta.data()) == 0x31464d52);
    CHECK(read_u32(meta.data()+12) == 253);
    CHECK(read_u64(meta.data()+16) == invalid_page_id);
    CHECK(read_u64(meta.data()+24) == 0);
    expect_error(error_code::invalid_argument,[&]{record_file::initialize(fm,bpm,fid,32);});
    CHECK(read_page(bpm,fid,1) == meta);
    {
        record_file file(fm,bpm,fid);
        CHECK(file.record_size() == 32);
        CHECK(file.record_count() == 0);
        expect_error(error_code::invalid_argument,[&]{file.insert(record_bytes(31));});
        CHECK(fm.get_file(fid).get_num_pages() == 2);
        CHECK(read_page(bpm,fid,1) == meta);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

void test_insert_and_reopen(temp_dir & tmp)
{
    file_manager fm;
    const fs::path path = tmp.path/"insert.db";
    file_id fid = create_file(fm,path);
    buffer_pool_manager bpm(fm,2);
    record_file::initialize(fm,bpm,fid,32);
    const slot_id slots = record_page_helper::capacity(32);
    std::vector<rid> ids;
    std::vector<record_bytes> expected;
    {
        record_file file(fm,bpm,fid);
        for(slot_id i = 0;i < slots * 2 + 1;++i)
        {
            expected.emplace_back(32,static_cast<std::byte>(i & 0xff));
            ids.push_back(file.insert(expected.back()));
            CHECK(ids.back().pid == 2 + i / slots);
            CHECK(ids.back().sid == i % slots);
            CHECK(file.record_count() == i + 1);
        }
        CHECK(fm.get_file(fid).get_num_pages() == 5);
        const page meta = read_page(bpm,fid,1);
        CHECK(read_u64(meta.data()+16) == 4);
        CHECK(read_u64(meta.data()+24) == ids.size());
    }
    bpm.check_invariants();
    bpm.close_file(fid);
    fid = fm.open_file(path.string());
    {
        record_file file(fm,bpm,fid);
        CHECK(file.record_count() == ids.size());
        for(std::size_t i = 0;i < ids.size();++i)
            CHECK(record_page_helper::read(read_page(bpm,fid,ids[i].pid),ids[i].sid) == expected[i]);
        const rid next = file.insert(record_bytes(32,std::byte{0xee}));
        CHECK(next.pid == 4 && next.sid == 1);
        CHECK(fm.get_file(fid).get_num_pages() == 5);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

void test_free_page_chain(temp_dir & tmp)
{
    file_manager fm;
    const file_id fid = create_file(fm,tmp.path/"chain.db");
    buffer_pool_manager bpm(fm,2);
    record_file::initialize(fm,bpm,fid,2048);
    const record_bytes bytes(2048,std::byte{0x42});
    // Prepare two non-full pages: head 2 -> 3 -> invalid.
    for(page_id pid = 2;pid <= 3;++pid)
    {
        auto guard = bpm.new_page(fid);
        CHECK(guard.page_no() == pid);
        page & p = *guard.get();
        record_page_helper::init(p,2048);
        CHECK(record_page_helper::insert(p,bytes) == 0);
        if(pid == 2)
            CHECK(record_page_helper::insert(p,bytes) == 1);
        write_u32(p.data()+20,1);
        write_u64(p.data()+24,pid == 2 ? 3 : invalid_page_id);
    }
    page meta = read_page(bpm,fid,1);
    write_u64(meta.data()+16,2);
    write_u64(meta.data()+24,3);
    write_page(bpm,fid,1,meta);
    {
        record_file file(fm,bpm,fid);
        const rid a = file.insert(bytes);
        CHECK(a.pid == 2 && a.sid == 2);
        CHECK(read_u64(read_page(bpm,fid,1).data()+16) == 3);
        const page full = read_page(bpm,fid,2);
        CHECK(read_u32(full.data()+20) == 0);
        CHECK(read_u64(full.data()+24) == invalid_page_id);
        const rid b = file.insert(bytes);
        CHECK(b.pid == 3 && b.sid == 1);
        const rid c = file.insert(bytes);
        CHECK(c.pid == 3 && c.sid == 2);
        CHECK(read_u64(read_page(bpm,fid,1).data()+16) == invalid_page_id);
        CHECK(file.record_count() == 6);
        CHECK(fm.get_file(fid).get_num_pages() == 4);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

void test_rejected_insert(temp_dir & tmp)
{
    file_manager fm;
    const file_id fid = create_file(fm,tmp.path/"rejected.db");
    buffer_pool_manager bpm(fm,2);
    record_file::initialize(fm,bpm,fid,32);
    record_file file(fm,bpm,fid);
    file.insert(record_bytes(32));
    const page original = read_page(bpm,fid,2);
    const page meta = read_page(bpm,fid,1);
    std::vector<page> bad_pages;
    for(page_id next : {page_id{0},page_id{1},page_id{2},page_id{3}})
    {
        page p = original;
        write_u64(p.data()+24,next);
        bad_pages.push_back(p);
    }
    page unlinked = original;
    write_u32(unlinked.data()+20,0);
    bad_pages.push_back(unlinked);
    for(const page & p : bad_pages)
    {
        write_page(bpm,fid,2,p);
        expect_error(error_code::invalid_format,[&]{file.insert(record_bytes(32));});
        CHECK(read_page(bpm,fid,1) == meta);
        CHECK(read_page(bpm,fid,2) == p);
        CHECK(fm.get_file(fid).get_num_pages() == 3);
    }
    write_page(bpm,fid,2,original);
    const rid id = file.insert(record_bytes(32));
    CHECK(id.pid == 2 && id.sid == 1);
    CHECK(file.record_count() == 2);
    bpm.check_invariants();
}

void test_buffer_exhaustion(temp_dir & tmp)
{
    for(slot_id size : {32u,8127u})
    {
        file_manager fm;
        const auto suffix = std::to_string(size)+".db";
        const file_id fid = create_file(fm,tmp.path/("exhausted"+suffix));
        const file_id other = create_file(fm,tmp.path/("other"+suffix));
        buffer_pool_manager bpm(fm,2);
        record_file::initialize(fm,bpm,fid,size);
        {
            record_file file(fm,bpm,fid);
            const record_bytes bytes(size,std::byte{0x55});
            const rid first = file.insert(bytes);
            const page meta = read_page(bpm,fid,1);
            const page data = read_page(bpm,fid,2);
            const page_id num_pages = fm.get_file(fid).get_num_pages();
            {
                auto pinned = bpm.new_page(other);
                expect_error(error_code::buffer_exhausted,[&]{file.insert(bytes);});
                expect_error(error_code::buffer_exhausted,[&]{file.erase(first);});
                CHECK(fm.get_file(fid).get_num_pages() == num_pages);
            }
            CHECK(read_page(bpm,fid,1) == meta);
            CHECK(read_page(bpm,fid,2) == data);
            CHECK(file.record_count() == 1);
            const rid id = file.insert(bytes);
            CHECK(id.pid == (size == 32 ? 2 : 3));
            CHECK(id.sid == (size == 32 ? 1 : 0));
            CHECK(file.record_count() == 2);
            file.erase(first);
            CHECK(file.record_count() == 1);
            CHECK(file.get(id) == bytes);
            CHECK(file.insert(bytes) == first);
            CHECK(file.record_count() == 2);
            if(size == 8127)
                CHECK(read_u64(read_page(bpm,fid,1).data()+16) == invalid_page_id);
        }
        bpm.check_invariants();
        bpm.close_file(other);
        bpm.close_file(fid);
    }
}

void test_metadata_corruption(temp_dir & tmp)
{
    file_manager fm;
    const file_id fid = create_file(fm,tmp.path/"metadata.db");
    buffer_pool_manager bpm(fm,2);
    record_file::initialize(fm,bpm,fid,32);
    const page original = read_page(bpm,fid,1);
    const std::pair<std::size_t,uint32_t> corruptions[] = {
        {0,0x31504d52},{4,2},{8,0},{8,8128},{12,0},{12,252},
        {16,0},{16,2},{24,1},{32,1},{8188,1}
    };
    for(const auto & change : corruptions)
    {
        page p = original;
        write_u32(p.data()+change.first,change.second);
        write_page(bpm,fid,1,p);
        expect_error(error_code::invalid_format,[&]{record_file file(fm,bpm,fid);});
        CHECK(read_page(bpm,fid,1) == p);
    }
    page huge_count = original;
    write_u64(huge_count.data()+24,std::numeric_limits<uint64_t>::max());
    write_page(bpm,fid,1,huge_count);
    expect_error(error_code::invalid_format,[&]{record_file file(fm,bpm,fid);});
    write_page(bpm,fid,1,original);
    {
        record_file file(fm,bpm,fid);
        page changed = original;
        write_u32(changed.data()+8,31);
        write_u32(changed.data()+12,261);
        write_page(bpm,fid,1,changed);
        expect_error(error_code::invalid_format,[&]{file.record_count();});
        expect_error(error_code::invalid_format,[&]{file.insert(record_bytes(32));});
        CHECK(read_page(bpm,fid,1) == changed);
        write_page(bpm,fid,1,original);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

void test_get_update_erase(temp_dir & tmp)
{
    file_manager fm;
    const fs::path path = tmp.path/"crud.db";
    file_id fid = create_file(fm,path);
    buffer_pool_manager bpm(fm,2);
    record_file::initialize(fm,bpm,fid,2048);
    std::vector<rid> ids;
    std::vector<record_bytes> expected;
    {
        record_file file(fm,bpm,fid);
        for(unsigned i = 0;i < 7;++i)
        {
            expected.emplace_back(2048,static_cast<std::byte>(i+1));
            ids.push_back(file.insert(expected.back()));
        }
        const page initial_meta = read_page(bpm,fid,1);
        expected[1].assign(2048,std::byte{0x77});
        file.update(ids[1],expected[1]);
        CHECK(file.get(ids[1]) == expected[1]);
        CHECK(read_page(bpm,fid,1) == initial_meta);
        record_bytes copy = file.get(ids[1]);
        copy[0] = std::byte{0};
        CHECK(file.get(ids[1]) == expected[1]);

        for(page_id pid : {page_id{0},page_id{1},page_id{5},invalid_page_id})
        {
            rid bad = ids[0];
            bad.pid = pid;
            expect_error(error_code::invalid_rid,[&]{file.get(bad);});
            expect_error(error_code::invalid_rid,[&]{file.update(bad,expected[0]);});
            expect_error(error_code::invalid_rid,[&]{file.erase(bad);});
        }
        rid bad_slot = ids[0];
        bad_slot.sid = 3;
        expect_error(error_code::invalid_rid,[&]{file.get(bad_slot);});
        expect_error(error_code::invalid_rid,[&]{file.update(bad_slot,expected[0]);});
        expect_error(error_code::invalid_rid,[&]{file.erase(bad_slot);});
        const page before_update = read_page(bpm,fid,2);
        for(slot_id size : {0u,2047u,2049u})
            expect_error(error_code::invalid_argument,[&]{file.update(ids[0],record_bytes(size));});
        CHECK(read_page(bpm,fid,2) == before_update);
        CHECK(read_page(bpm,fid,1) == initial_meta);

        // A full page becomes the free-list head; lazy remove keeps its payload bytes.
        file.erase(ids[0]);
        CHECK(file.record_count() == 6);
        const page deleted = read_page(bpm,fid,2);
        CHECK(std::equal(before_update.begin()+65,before_update.begin()+2113,deleted.begin()+65));
        CHECK(read_u64(read_page(bpm,fid,1).data()+16) == 2);
        CHECK(read_u32(deleted.data()+20) == 1);
        CHECK(read_u64(deleted.data()+24) == 4);
        const page after_delete_meta = read_page(bpm,fid,1);
        expect_error(error_code::record_not_found,[&]{file.get(ids[0]);});
        expect_error(error_code::record_not_found,[&]{file.update(ids[0],expected[0]);});
        expect_error(error_code::record_not_found,[&]{file.erase(ids[0]);});
        CHECK(read_page(bpm,fid,1) == after_delete_meta);
        CHECK(read_page(bpm,fid,2) == deleted);

        // A second deletion from the same page must not link it twice.
        file.erase(ids[2]);
        CHECK(file.record_count() == 5);
        CHECK(read_u64(read_page(bpm,fid,2).data()+24) == 4);
        file.erase(ids[3]);
        CHECK(file.record_count() == 4);
        CHECK(read_u64(read_page(bpm,fid,1).data()+16) == 3);
        CHECK(read_u64(read_page(bpm,fid,3).data()+24) == 2);
        for(unsigned i : {3u,0u,2u})
        {
            expected[i].assign(2048,static_cast<std::byte>(0x80+i));
            CHECK(file.insert(expected[i]) == ids[i]);
        }
        CHECK(file.record_count() == 7);
        CHECK(fm.get_file(fid).get_num_pages() == 5);
        for(std::size_t i = 0;i < ids.size();++i)
            CHECK(file.get(ids[i]) == expected[i]);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
    fid = fm.open_file(path.string());
    {
        record_file file(fm,bpm,fid);
        CHECK(file.record_count() == 7);
        for(std::size_t i = 0;i < ids.size();++i)
            CHECK(file.get(ids[i]) == expected[i]);
        for(const rid & id : ids)
            file.erase(id);
        CHECK(file.record_count() == 0);
        CHECK(fm.get_file(fid).get_num_pages() == 5);
        file.insert(expected[0]);
        CHECK(file.record_count() == 1);
        CHECK(fm.get_file(fid).get_num_pages() == 5);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

void test_mismatched_data_layout(temp_dir & tmp)
{
    file_manager fm;
    const file_id fid = create_file(fm,tmp.path/"data_layout.db");
    buffer_pool_manager bpm(fm,2);
    record_file::initialize(fm,bpm,fid,32);
    {
        record_file file(fm,bpm,fid);
        const rid id = file.insert(record_bytes(32));
        page wrong{};
        record_page_helper::init(wrong,31);
        record_page_helper::insert(wrong,record_bytes(31));
        write_page(bpm,fid,id.pid,wrong);
        expect_error(error_code::invalid_format,[&]{file.get(id);});
        expect_error(error_code::invalid_format,[&]{file.update(id,record_bytes(32));});
        expect_error(error_code::invalid_format,[&]{file.erase(id);});
        CHECK(read_page(bpm,fid,id.pid) == wrong);
        CHECK(file.record_count() == 1);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

void test_rejected_erase(temp_dir & tmp)
{
    file_manager fm;
    const file_id fid = create_file(fm,tmp.path/"erase_rejected.db");
    buffer_pool_manager bpm(fm,2);
    record_file::initialize(fm,bpm,fid,2048);
    {
        record_file file(fm,bpm,fid);
        const record_bytes bytes(2048,std::byte{0x66});
        const rid id = file.insert(bytes);
        file.insert(bytes);
        file.insert(bytes);
        const page full_meta = read_page(bpm,fid,1);
        const page full_data = read_page(bpm,fid,2);
        std::vector<std::pair<page,page>> corruptions;
        for(unsigned i = 0;i < 4;++i)
        {
            page meta = full_meta;
            page data = full_data;
            if(i == 0)
                write_u32(data.data()+20,1);
            else if(i == 1)
                write_u64(data.data()+24,2);
            else if(i == 2)
                write_u64(meta.data()+16,2);
            else
                write_u64(meta.data()+24,0);
            corruptions.emplace_back(meta,data);
        }
        for(const auto & bad : corruptions)
        {
            write_page(bpm,fid,1,bad.first);
            write_page(bpm,fid,2,bad.second);
            expect_error(error_code::invalid_format,[&]{file.erase(id);});
            CHECK(read_page(bpm,fid,1) == bad.first);
            CHECK(read_page(bpm,fid,2) == bad.second);
        }
        write_page(bpm,fid,1,full_meta);
        write_page(bpm,fid,2,full_data);
        file.erase(id);
        const page free_meta = read_page(bpm,fid,1);
        const page free_data = read_page(bpm,fid,2);
        rid remaining = id;
        remaining.sid = 1;
        for(unsigned i = 0;i < 5;++i)
        {
            page meta = free_meta;
            page data = free_data;
            if(i == 0)
                write_u32(data.data()+20,0);
            else if(i == 1)
                write_u64(meta.data()+16,invalid_page_id);
            else
                write_u64(data.data()+24,i-1); // Reserved page, self-link or out-of-range.
            write_page(bpm,fid,1,meta);
            write_page(bpm,fid,2,data);
            expect_error(error_code::invalid_format,[&]{file.erase(remaining);});
            CHECK(read_page(bpm,fid,1) == meta);
            CHECK(read_page(bpm,fid,2) == data);
        }
        write_page(bpm,fid,1,free_meta);
        write_page(bpm,fid,2,free_data);
        CHECK(file.insert(bytes) == id);
        CHECK(file.record_count() == 3);
    }
    bpm.check_invariants();
    bpm.close_file(fid);
}

}

int main()
{
    try
    {
        temp_dir tmp;
        test_initialize(tmp);
        test_insert_and_reopen(tmp);
        test_free_page_chain(tmp);
        test_rejected_insert(tmp);
        test_buffer_exhaustion(tmp);
        test_metadata_corruption(tmp);
        test_get_update_erase(tmp);
        test_mismatched_data_layout(tmp);
        test_rejected_erase(tmp);
    }
    catch(const std::exception & error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if(failures != 0)
        return 1;
    std::cout << "record file tests passed\n";
}
