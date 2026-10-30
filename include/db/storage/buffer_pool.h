#pragma once
#include <db/common/types.h>
#include <db/common/error.h>
#include <db/storage/file_manager.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace db
{

constexpr frame_id invalid_frame_id = std::numeric_limits<frame_id>::max();

class buffer_pool_manager;

enum class access_type
{
    read,
    write
};

struct buffer_pool_stats
{
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t writebacks = 0;
};

struct page_key
{
    file_id fid = 0;
    page_id pid = 0;
    bool operator==(const page_key & pk)const noexcept;
};

struct hash_page_key
{
    std::size_t operator()(const page_key & k)const noexcept;
};

struct frame
{
    page _page{};
    file_id fid = 0;
    page_id pid = 0;
    std::uint32_t pin_count = 0;
    bool valid = false;
    bool dirty = false;
    void reset()noexcept;
    void set(file_id f,page_id p,std::uint32_t pc = 0,bool v = true,bool d = false)noexcept;
};

class page_table
{
private:
    using map_type = std::unordered_map<page_key,frame_id,hash_page_key>;
    map_type hash_table;
public:
    page_table();
    frame_id query(const page_key & k)const;
    frame_id query(file_id fid,page_id pid)const;
    bool contains(const page_key & k)const;
    bool contains(file_id fid,page_id pid)const;
    bool insert(const page_key & k,frame_id f);
    bool insert(file_id fid,page_id pid,frame_id f);
    bool erase(const page_key & k);
    bool erase(file_id fid,page_id pid);
    void clear()noexcept;
    bool empty()const noexcept;
    std::size_t size()const noexcept;
    void reserve(std::size_t cap);
    friend class buffer_pool_manager;
};

class free_list
{
private:
    std::vector<frame_id> next;
    std::vector<bool> linked; // 1 for held, 0 for given out
    frame_id head;
    std::size_t count;
    friend class buffer_pool_manager;
public:
    free_list(std::size_t cap);
    bool empty()const noexcept;
    std::size_t size()const noexcept;
    bool contains(frame_id fid)const noexcept;
    frame_id get_empty_frame()noexcept;
    bool push(frame_id fid)noexcept;
    void clear()noexcept;
};

class LRU_list;

class _List
{
private:
    std::vector<frame_id> prev;
    std::vector<frame_id> next;
    std::vector<bool> linked;
    frame_id head;
    frame_id tail;
    std::size_t count;
    friend class LRU_list;
    friend class buffer_pool_manager;
public:
    explicit _List(std::size_t cap);
    bool empty()const noexcept;
    std::size_t size()const noexcept;
    bool contains(frame_id fid)const noexcept;
    bool push_front(frame_id fid)noexcept;
    bool move_front(frame_id id)noexcept;
    bool erase(frame_id fid)noexcept;
    frame_id pop_back()noexcept;
    void clear()noexcept;
};


class LRU_list
{
private:
    _List list;
    std::size_t _cap;
    std::vector<bool> pinned;
    friend class _List;
    friend class buffer_pool_manager;
public:
    LRU_list(std::size_t cap = 0);
    bool empty()const noexcept;
    std::size_t size()const noexcept;
    bool contains(frame_id fid)const noexcept;
    frame_id victim(bool remove = true,frame_id fid = invalid_frame_id)noexcept;
    bool record_access(frame_id fid)noexcept;
    void clear()noexcept;
    void pin(frame_id fid)noexcept;
    void unpin(frame_id fid)noexcept;
};



class page_guard
{
private:
    frame_id f;
    buffer_pool_manager* bpm;
    bool can_write;
    page_guard(frame_id fid,buffer_pool_manager* bpm_,bool can_write_)noexcept;
    page_guard(const page_guard & ppp)=delete;
    page_guard & operator=(const page_guard & ppp)=delete;
public:
    ~page_guard()noexcept;
    page_guard(page_guard && ppp)noexcept;
    page_guard & operator=(page_guard && ppp)noexcept;
    const page * view()const noexcept;
    page* get()noexcept;
    void release()noexcept;
    frame_id frame()const noexcept;
    file_id file()const noexcept;
    page_id page_no()const noexcept;
    bool writable()const noexcept;
    explicit operator bool()const noexcept;
    friend class buffer_pool_manager;
};

class buffer_pool_manager
{
private:
    friend class LRU_list;
    friend class _List;
    file_manager & fm;
    std::vector<frame> frames;
    page_table table;
    // Contains exactly the resident pages of each file; empty sets are removed.
    std::unordered_map<file_id, std::unordered_set<page_id>> file_pages;
    free_list free_frames;
    LRU_list lru;
    buffer_pool_stats stat;

    buffer_pool_manager(const buffer_pool_manager & bpm)=delete;
    buffer_pool_manager & operator=(const buffer_pool_manager & bpm)=delete;
    buffer_pool_manager(buffer_pool_manager && bpm)=delete;
    buffer_pool_manager & operator=(buffer_pool_manager && bpm)=delete;

    frame * get_frame(frame_id fid)noexcept;
    const frame * get_frame(frame_id fid)const noexcept;
    frame_id find_frame(file_id fid,page_id pid)const;
    frame_id acquire_frame();
    void pin(frame_id fid)noexcept;
    void unpin(frame_id fid)noexcept;
    void mark_dirty(frame_id fid)noexcept;
    void write_back(frame & fr);
    void add_mapping(frame_id fid);
    void remove_mapping(frame_id fid);
public:
    buffer_pool_manager(file_manager & fm_,std::size_t frame_count);
    ~buffer_pool_manager()noexcept;

    // Page 0 belongs exclusively to file_manager and is never cached.
    page_guard fetch_page(file_id fid,page_id pid,access_type access = access_type::read);
    page_guard new_page(file_id fid);

    // Flushes preserve residency. Counts report successful dirty-page writes.
    bool flush_page(file_id fid,page_id pid);
    std::size_t flush_file(file_id fid);
    std::size_t flush_all();
    // Missing or pinned pages cannot be removed. Discard abandons dirty data.
    bool evict_page(file_id fid,page_id pid);
    bool discard_page(file_id fid,page_id pid);
    std::size_t flush_and_invalidate_file(file_id fid);
    // Preferred close path: reject pinned pages, flush/invalidate, then close.
    void close_file(file_id fid);
    // Requires all guards released; flushes and resets the pool, keeping stats.
    void shutdown();

    std::size_t frame_count()const noexcept;
    std::size_t free_frame_count()const noexcept;
    std::size_t cached_page_count()const noexcept;
    const buffer_pool_stats & stats()const noexcept;
    void check_invariants()const;

    friend class page_guard;
};

}
