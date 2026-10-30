#include "db/storage/buffer_pool.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace db;

namespace {

int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << ':' << __LINE__ \
                      << ": CHECK failed: " #condition << '\n'; \
            ++failures; \
        } \
    } while (false)

template<class F>
bool throws_code(error_code code, F && f)
{
    try { f(); }
    catch (const db_error & error) { return error.code() == code; }
    return false;
}

struct temp_dir
{
    fs::path path;
    temp_dir()
    {
        std::string pattern = (fs::temp_directory_path() / "idbs_bpm_XXXXXX").string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        char * created = ::mkdtemp(name.data());
        if (!created)
            throw std::runtime_error("cannot create buffer pool test directory");
        path = created;
    }
    ~temp_dir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

file_id create_file(file_manager & fm, const fs::path & path)
{
    fm.create_file(path.string());
    return fm.open_file(path.string());
}

page pattern_page(unsigned value)
{
    page result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::byte>((value * 37 + i * 13) & 0xff);
    return result;
}

void test_helpers()
{
    page_table table;
    CHECK(table.empty());
    CHECK(table.query(1, 2) == invalid_frame_id);
    CHECK(table.insert(1, 2, 0));
    CHECK(!table.insert(1, 2, 1));
    CHECK(table.insert(2, 2, 1));
    CHECK(table.query(1, 2) == 0 && table.query(2, 2) == 1);
    CHECK(table.erase(1, 2) && !table.erase(1, 2));
    table.clear();
    CHECK(table.empty());

    CHECK(throws_code(error_code::invalid_argument, [] { free_list list(0); }));
    free_list free(3);
    CHECK(free.get_empty_frame() == 0);
    CHECK(free.get_empty_frame() == 1);
    CHECK(free.get_empty_frame() == 2);
    CHECK(free.empty() && free.get_empty_frame() == invalid_frame_id);
    CHECK(free.push(1) && !free.push(1));
    CHECK(!free.push(invalid_frame_id));
    CHECK(free.get_empty_frame() == 1);
    free.clear();
    CHECK(free.size() == 3 && free.contains(0) && free.contains(2));

    _List list(3);
    CHECK(list.push_front(0));
    CHECK(list.push_front(1));
    CHECK(list.push_front(2));
    CHECK(!list.push_front(2));
    CHECK(list.erase(1));
    CHECK(list.move_front(0));
    CHECK(list.pop_back() == 2);
    CHECK(list.pop_back() == 0);
    CHECK(list.pop_back() == invalid_frame_id);
    CHECK(list.push_front(1));
    CHECK(list.pop_back() == 1 && !list.erase(1));
    list.clear();

    LRU_list lru(3);
    CHECK(lru.record_access(0));
    CHECK(lru.record_access(1));
    CHECK(lru.record_access(2));
    lru.pin(0);
    CHECK(lru.victim(false) == 1);
    CHECK(lru.victim(true, 0) == invalid_frame_id);
    CHECK(lru.victim(true, 99) == invalid_frame_id);
    CHECK(lru.record_access(1));
    CHECK(lru.victim() == 2);
    CHECK(lru.victim() == 1);
    CHECK(lru.victim() == invalid_frame_id);
    lru.unpin(0);
    CHECK(lru.victim() == 0);
    lru.clear();
    CHECK(lru.empty() && !lru.record_access(invalid_frame_id));
}

void test_guard_and_stats(temp_dir & tmp)
{
    static_assert(!std::is_copy_constructible_v<page_guard>);
    static_assert(!std::is_copy_assignable_v<page_guard>);
    static_assert(std::is_nothrow_move_constructible_v<page_guard>);
    static_assert(std::is_nothrow_move_assignable_v<page_guard>);
    file_manager fm;
    file_id fid = create_file(fm, tmp.path / "guards.db");
    CHECK(throws_code(error_code::invalid_argument, [&] { buffer_pool_manager bpm(fm, 0); }));
    CHECK(throws_code(error_code::invalid_argument, [&] {
        buffer_pool_manager bpm(fm, static_cast<std::size_t>(invalid_frame_id) + 1);
    }));
    buffer_pool_manager bpm(fm, 2);
    CHECK(bpm.frame_count() == 2 && bpm.free_frame_count() == 2);
    bpm.check_invariants();
    auto first = bpm.new_page(fid);
    const page_id p1 = first.page_no();
    const frame_id f1 = first.frame();
    CHECK(first && first.writable() && first.file() == fid);
    CHECK(*first.view() == page{});
    CHECK(bpm.stats().hits == 0 && bpm.stats().misses == 0);

    auto moved = std::move(first);
    CHECK(!first && first.view() == nullptr && first.get() == nullptr);
    CHECK(first.frame() == invalid_frame_id);
    CHECK(first.file() == 0 && first.page_no() == 0 && !first.writable());
    CHECK(moved.frame() == f1);
    auto same = bpm.fetch_page(fid, p1);
    CHECK(same.frame() == f1 && same.get() == nullptr && !same.writable());
    CHECK(bpm.stats().hits == 1);
    moved.release();
    CHECK(!bpm.evict_page(fid, p1));
    same.release();
    same.release();
    CHECK(bpm.evict_page(fid, p1));
    CHECK(bpm.stats().evictions == 1 && bpm.stats().writebacks == 0);

    auto loaded = bpm.fetch_page(fid, p1);
    CHECK(bpm.stats().misses == 1);
    auto destination = bpm.new_page(fid);
    const page_id p2 = destination.page_no();
    destination = std::move(loaded);
    CHECK(!loaded && destination.page_no() == p1);
    CHECK(bpm.evict_page(fid, p2)); // assignment released destination's old pin
    page_guard & alias = destination;
    destination = std::move(alias);
    CHECK(destination && destination.page_no() == p1);
    destination.release();
    bpm.check_invariants();
    bpm.shutdown();
    CHECK(bpm.cached_page_count() == 0 && bpm.free_frame_count() == 2);
    CHECK(bpm.stats().hits == 1 && bpm.stats().misses == 1);
    bpm.shutdown();
    bpm.check_invariants();
}

void test_dirty_eviction_and_reopen(temp_dir & tmp)
{
    file_manager fm;
    fs::path path = tmp.path / "ten_pages.db";
    file_id fid = create_file(fm, path);
    std::vector<page_id> ids;
    {
        buffer_pool_manager bpm(fm, 3);
        for (unsigned i = 0; i < 10; ++i) {
            auto g = bpm.new_page(fid);
            ids.push_back(g.page_no());
            *g.get() = pattern_page(i);
            bpm.check_invariants();
        }
        CHECK(bpm.stats().evictions == 7 && bpm.stats().writebacks == 7);
        CHECK(bpm.flush_all() == 3 && bpm.flush_all() == 0);
        for (unsigned i = 0; i < ids.size(); ++i) {
            auto g = bpm.fetch_page(fid, ids[i]);
            CHECK(*g.view() == pattern_page(i));
            bpm.check_invariants();
        }
        bpm.shutdown();
    }
    fm.close_file(fid);
    fid = fm.open_file(path.string());
    CHECK(fm.get_file(fid).get_num_pages() == 11);
    for (unsigned i = 0; i < ids.size(); ++i) {
        page disk{};
        fm.get_file(fid).read_page(ids[i], disk);
        CHECK(disk == pattern_page(i));
    }
}

void test_lru_and_exhaustion(temp_dir & tmp)
{
    file_manager fm;
    file_id fid = create_file(fm, tmp.path / "lru.db");
    buffer_pool_manager bpm(fm, 3);
    std::vector<page_id> ids;
    for (int i = 0; i < 3; ++i) {
        auto g = bpm.new_page(fid);
        ids.push_back(g.page_no());
    }
    { auto g = bpm.fetch_page(fid, ids[0]); }
    const auto hits = bpm.stats().hits;
    { auto g = bpm.new_page(fid); }
    CHECK(!bpm.flush_page(fid, ids[1])); // oldest access was evicted
    CHECK(bpm.flush_page(fid, ids[0]));
    { auto g = bpm.fetch_page(fid, ids[0]); }
    CHECK(bpm.stats().hits == hits + 1);

    auto a = bpm.fetch_page(fid, ids[0]);
    auto b = bpm.fetch_page(fid, ids[1]);
    auto c = bpm.fetch_page(fid, ids[2]);
    const page_id before = fm.get_file(fid).get_num_pages();
    CHECK(throws_code(error_code::buffer_exhausted, [&] { auto g = bpm.new_page(fid); }));
    CHECK(fm.get_file(fid).get_num_pages() == before);
    CHECK(throws_code(error_code::invalid_page, [&] { auto g = bpm.fetch_page(fid, 0); }));
    CHECK(!bpm.evict_page(fid, ids[0]) && !bpm.discard_page(fid, ids[0]));
    CHECK(throws_code(error_code::resource_busy, [&] { bpm.shutdown(); }));
    CHECK(throws_code(error_code::resource_busy, [&] { bpm.flush_and_invalidate_file(fid); }));
    CHECK(a && b && c && a.page_no() == ids[0]);
    bpm.check_invariants();
    c.release();
    { auto g = bpm.fetch_page(fid, before - 1); }
    bpm.check_invariants();
}

void test_flush_and_discard(temp_dir & tmp)
{
    file_manager fm;
    file_id fid = create_file(fm, tmp.path / "flush.db");
    buffer_pool_manager bpm(fm, 1);
    auto g = bpm.new_page(fid);
    page_id pid = g.page_no();
    page * writable = g.get();
    *writable = pattern_page(10);
    CHECK(bpm.flush_page(fid, pid));
    page disk{};
    fm.get_file(fid).read_page(pid, disk);
    CHECK(disk == pattern_page(10));
    *writable = pattern_page(11); // no second get(): dirty must survive flush
    g.release();
    CHECK(bpm.flush_file(fid) == 1 && bpm.flush_file(fid) == 0);
    CHECK(bpm.cached_page_count() == 1 && bpm.free_frame_count() == 0);
    fm.get_file(fid).read_page(pid, disk);
    CHECK(disk == pattern_page(11));
    { auto edited = bpm.fetch_page(fid, pid, access_type::write); *edited.get() = pattern_page(12); }
    auto writebacks = bpm.stats().writebacks;
    auto evictions = bpm.stats().evictions;
    CHECK(bpm.discard_page(fid, pid));
    CHECK(bpm.stats().writebacks == writebacks && bpm.stats().evictions == evictions);
    CHECK(!bpm.discard_page(fid, pid) && !bpm.evict_page(fid, pid));
    CHECK(!bpm.flush_page(fid, pid));
    auto reloaded = bpm.fetch_page(fid, pid);
    CHECK(*reloaded.view() == pattern_page(11));
    reloaded.release();
    CHECK(bpm.evict_page(fid, pid));
    bpm.check_invariants();
}

void test_file_index_and_close(temp_dir & tmp)
{
    file_manager fm;
    // Ensure a file ID differs from the frame ID chosen for it.
    file_id dummy = create_file(fm, tmp.path / "dummy.db");
    fm.close_file(dummy);
    fs::path a_path = tmp.path / "index_a.db";
    fs::path b_path = tmp.path / "index_b.db";
    file_id a = create_file(fm, a_path);
    file_id b = create_file(fm, b_path);
    buffer_pool_manager bpm(fm, 5);
    std::vector<page_id> a_pages;
    page_id b_page;
    {
        auto g = bpm.new_page(b);
        b_page = g.page_no();
        CHECK(g.file() == b && g.frame() != b);
        *g.get() = pattern_page(20);
    }
    for (unsigned i = 0; i < 2; ++i) {
        auto g = bpm.new_page(a);
        a_pages.push_back(g.page_no());
        *g.get() = pattern_page(21 + i);
    }
    CHECK(a_pages[0] == b_page);
    bpm.check_invariants();
    CHECK(bpm.flush_file(a) == 2);
    page disk{};
    fm.get_file(b).read_page(b_page, disk);
    CHECK(disk == page{}); // flushing A must not write B
    CHECK(bpm.flush_file(9999) == 0 && bpm.flush_and_invalidate_file(9999) == 0);
    auto pinned = bpm.fetch_page(a, a_pages[0]);
    auto writes = bpm.stats().writebacks;
    CHECK(throws_code(error_code::resource_busy, [&] { bpm.flush_and_invalidate_file(a); }));
    CHECK(bpm.cached_page_count() == 3 && bpm.stats().writebacks == writes);
    pinned.release();
    CHECK(bpm.flush_and_invalidate_file(a) == 2);
    CHECK(bpm.cached_page_count() == 1 && !bpm.flush_page(a, a_pages[0]));
    bpm.check_invariants();
    fm.close_file(a);
    a = fm.open_file(a_path.string());
    CHECK(bpm.flush_and_invalidate_file(a) == 0);
    { auto g = bpm.fetch_page(a, a_pages[0]); CHECK(*g.view() == pattern_page(21)); }
    CHECK(bpm.flush_and_invalidate_file(b) == 1);
    fm.close_file(b);
    b = fm.open_file(b_path.string());
    fm.get_file(b).read_page(b_page, disk);
    CHECK(disk == pattern_page(20));
    CHECK(bpm.flush_and_invalidate_file(a) == 1);
    CHECK(bpm.cached_page_count() == 0 && bpm.free_frame_count() == 5);
    bpm.check_invariants();
}

void test_read_and_write_failures(temp_dir & tmp)
{
    file_manager fm;
    file_id fid = create_file(fm, tmp.path / "errors.db");
    file_handle & fh = fm.get_file(fid);
    page_id pid = fh.allocate_page();
    page_id other = fh.allocate_page();
    buffer_pool_manager bpm(fm, 1);
    CHECK(throws_code(error_code::invalid_file_handle, [&] { auto g = bpm.fetch_page(9999, pid); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { auto g = bpm.new_page(9999); }));
    CHECK(bpm.free_frame_count() == 1);
    CHECK(throws_code(error_code::invalid_page, [&] { auto g = bpm.fetch_page(fid, 9999); }));
    CHECK(bpm.free_frame_count() == 1);
    bpm.check_invariants();
    { auto g = bpm.fetch_page(fid, pid, access_type::write); *g.get() = pattern_page(30); }
    auto evictions = bpm.stats().evictions;
    auto writes = bpm.stats().writebacks;
    fh.close(); // inject a real write failure while keeping cached metadata
    CHECK(throws_code(error_code::invalid_file_handle, [&] { bpm.flush_page(fid, pid); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { bpm.flush_file(fid); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { bpm.flush_and_invalidate_file(fid); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { auto g = bpm.fetch_page(fid, other); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { bpm.evict_page(fid, pid); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { bpm.shutdown(); }));
    CHECK(bpm.stats().evictions == evictions && bpm.stats().writebacks == writes);
    CHECK(bpm.cached_page_count() == 1 && bpm.free_frame_count() == 0);
    CHECK(throws_code(error_code::invalid_file_handle, [&] { auto g = bpm.fetch_page(fid, pid); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { auto g = bpm.new_page(fid); }));
    bpm.check_invariants();
    CHECK(bpm.discard_page(fid, pid)); // intentionally abandon this failed write
    bpm.check_invariants();
    fm.close_file(fid);
    CHECK(bpm.flush_file(fid) == 0);
    bpm.shutdown();
}

void test_reserved_header_and_close(temp_dir & tmp)
{
    file_manager fm;
    const auto path = tmp.path / "reserved_header.db";
    file_id fid = create_file(fm, path);
    buffer_pool_manager bpm(fm, 1);
    auto guard = bpm.new_page(fid);
    const auto pid = guard.page_no();
    *guard.get() = pattern_page(55);
    const auto before = bpm.stats();

    CHECK(throws_code(error_code::invalid_page, [&] { auto g = bpm.fetch_page(fid, 0); }));
    CHECK(throws_code(error_code::invalid_page, [&] { bpm.flush_page(fid, 0); }));
    CHECK(throws_code(error_code::invalid_page, [&] { bpm.evict_page(fid, 0); }));
    CHECK(throws_code(error_code::invalid_page, [&] { bpm.discard_page(fid, 0); }));
    CHECK(throws_code(error_code::invalid_argument, [&] {
        auto g = bpm.fetch_page(fid, pid, static_cast<access_type>(99));
    }));
    CHECK(throws_code(error_code::resource_busy, [&] { bpm.close_file(fid); }));
    CHECK(fm.get_file(fid).is_open() && guard && *guard.view() == pattern_page(55));
    guard.release();

    // Reject bad input before it can evict or flush the sole dirty frame.
    try {
        auto g = bpm.fetch_page(fid, 12345);
        CHECK(false);
    } catch (const db_error& error) {
        CHECK(error.code() == error_code::invalid_page);
        CHECK(error.context().object_id == fid && error.context().page == 12345);
        CHECK(error.context().operation == "buffer_pool.fetch_page");
    }
    CHECK(bpm.stats().hits == before.hits && bpm.stats().misses == before.misses);
    CHECK(bpm.stats().evictions == before.evictions && bpm.stats().writebacks == before.writebacks);
    CHECK(bpm.cached_page_count() == 1);
    bpm.check_invariants();

    bpm.close_file(fid);
    CHECK(bpm.cached_page_count() == 0 && bpm.free_frame_count() == 1);
    CHECK(throws_code(error_code::invalid_file_handle, [&] { fm.get_file(fid); }));
    CHECK(throws_code(error_code::invalid_file_handle, [&] { bpm.close_file(fid); }));
    fid = fm.open_file(path.string());
    CHECK(fm.get_file(fid).get_num_pages() == 2);
    { auto g = bpm.fetch_page(fid, pid); CHECK(*g.view() == pattern_page(55)); }
    bpm.close_file(fid);
    bpm.shutdown();
}

void test_destructor_flush(temp_dir & tmp)
{
    file_manager fm;
    fs::path path = tmp.path / "destructor.db";
    file_id fid = create_file(fm, path);
    page_id pid;
    {
        buffer_pool_manager bpm(fm, 1);
        auto g = bpm.new_page(fid);
        pid = g.page_no();
        *g.get() = pattern_page(40);
    }
    fm.close_file(fid);
    fid = fm.open_file(path.string());
    page disk{};
    fm.get_file(fid).read_page(pid, disk);
    CHECK(disk == pattern_page(40));
    file_id bad = create_file(fm, tmp.path / "destructor_bad.db");
    file_id good = create_file(fm, tmp.path / "destructor_good.db");
    page_id good_pid;
    {
        buffer_pool_manager bpm(fm, 2);
        { auto g = bpm.new_page(bad); *g.get() = pattern_page(41); }
        {
            auto g = bpm.new_page(good);
            good_pid = g.page_no();
            *g.get() = pattern_page(42);
        }
        fm.get_file(bad).close();
    } // a failing frame must not prevent flushing the other
    fm.get_file(good).read_page(good_pid, disk);
    CHECK(disk == pattern_page(42));
}

void test_randomized_index_lifecycle(temp_dir & tmp)
{
    file_manager fm;
    const std::size_t file_count = 3;
    const std::size_t pages_per_file = 7;
    std::vector<file_id> files;
    std::vector<std::vector<page_id>> ids(file_count);
    std::vector<std::vector<page>> expected(file_count);
    for (std::size_t f = 0; f < file_count; ++f) {
        files.push_back(create_file(fm, tmp.path / ("random_" + std::to_string(f) + ".db")));
        for (std::size_t p = 0; p < pages_per_file; ++p) {
            page_id pid = fm.get_file(files[f]).allocate_page();
            page data = pattern_page(static_cast<unsigned>(f * 20 + p));
            fm.get_file(files[f]).write_page(pid, data);
            ids[f].push_back(pid);
            expected[f].push_back(data);
        }
    }
    buffer_pool_manager bpm(fm, 4);
    std::mt19937 random(20261027);
    for (unsigned step = 0; step < 500; ++step) {
        std::size_t f = random() % file_count;
        std::size_t p = random() % pages_per_file;
        switch (random() % 6) {
        case 0: {
            auto g = bpm.fetch_page(files[f], ids[f][p], access_type::write);
            *g.get() = pattern_page(step + 100);
            expected[f][p] = *g.view();
            break;
        }
        case 1:
            bpm.flush_file(files[f]);
            break;
        case 2:
            bpm.flush_and_invalidate_file(files[f]);
            break;
        case 3:
            bpm.evict_page(files[f], ids[f][p]);
            break;
        case 4:
            bpm.flush_page(files[f], ids[f][p]); // persist before discard
            bpm.discard_page(files[f], ids[f][p]);
            break;
        default: {
            auto g = bpm.fetch_page(files[f], ids[f][p]);
            CHECK(*g.view() == expected[f][p]);
            break;
        }
        }
        bpm.check_invariants();
    }
    bpm.shutdown();
    bpm.check_invariants();
    for (std::size_t f = 0; f < file_count; ++f) {
        for (std::size_t p = 0; p < pages_per_file; ++p) {
            page disk{};
            fm.get_file(files[f]).read_page(ids[f][p], disk);
            CHECK(disk == expected[f][p]);
        }
    }
}

}

int main()
{
    try {
        temp_dir tmp;
        test_helpers();
        test_guard_and_stats(tmp);
        test_dirty_eviction_and_reopen(tmp);
        test_lru_and_exhaustion(tmp);
        test_flush_and_discard(tmp);
        test_file_index_and_close(tmp);
        test_read_and_write_failures(tmp);
        test_reserved_header_and_close(tmp);
        test_destructor_flush(tmp);
        test_randomized_index_lifecycle(tmp);
    }
    catch (const std::exception & e) {
        std::cerr << "unexpected exception: " << e.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "buffer_pool tests passed (10 groups, 500 randomized operations)\n";
    return 0;
}
