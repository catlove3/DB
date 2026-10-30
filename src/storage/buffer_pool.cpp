#include "db/storage/buffer_pool.h"
#include "db/common/types.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace db
{

namespace {

db_error pool_error(error_code code, const char* message, const char* operation,
                    std::optional<file_id> fid = {}, std::optional<page_id> pid = {})
{
    error_context context;
    context.operation = operation;
    context.object_id = fid;
    context.page = pid;
    return db_error(code, message, std::move(context));
}

void require_data_page(file_id fid, page_id pid, const char* operation)
{
    if (pid == 0)
        throw pool_error(error_code::invalid_page,
                         "file header page cannot be cached", operation, fid, pid);
}

std::size_t checked_frame_count(std::size_t count)
{
    if (count == 0 || count > invalid_frame_id)
        throw pool_error(error_code::invalid_argument,
                         "invalid buffer pool frame count", "buffer_pool.capacity");
    return count;
}

}

bool page_key::operator==(const page_key & k)const noexcept
{
    return fid == k.fid && pid == k.pid;
}


std::size_t hash_page_key::operator()(const page_key & k)const noexcept
{
    uint64_t x = k.fid * 0x9e3779b97f4a7c15ULL;
    x ^= k.pid + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;

    return std::size_t(x);
}


void frame::reset()noexcept
{
    fid = 0;
    pid = 0;
    pin_count = 0;
    valid = false;
    dirty = false;
}

void frame::set(file_id f,page_id p,std::uint32_t pc,bool v,bool d)noexcept
{
    fid = f;
    pid = p;
    pin_count = pc;
    valid = v;
    dirty = d;
}


/*

Class: PAGE TABLE

Docs:
    Maps each page key (file_id, page_id) to its corresponding frame_id.
    Uses a hash table for efficient page lookup in the buffer pool.

Class Methods:
    page_table()                                  -> initialize an empty page table
    query(file_id fid, page_id pid)               -> return the corresponding frame_id;
                                                     invalid_frame_id if not found
    query(const page_key& pk)                     -> return the corresponding frame_id;
                                                     invalid_frame_id if not found
    contains(file_id fid, page_id pid)            -> whether the page exists in the table
    contains(const page_key& pk)                  -> whether the page exists in the table
    insert(file_id fid, page_id pid, frame_id frid)-> insert a page-to-frame mapping;
                                                     false if the page already exists
    insert(const page_key& pk, frame_id fid)      -> insert a page-to-frame mapping;
                                                     false if the page already exists
    erase(file_id fid, page_id pid)               -> remove the page mapping;
                                                     false if the page does not exist
    erase(const page_key& pk)                     -> remove the page mapping;
                                                     false if the page does not exist
    clear()                                       -> remove all page mappings
    empty()                                       -> whether the page table is empty
    size()                                        -> number of page mappings
    reserve(std::size_t cap)                      -> reserve buckets for at least cap elements

*/

page_table::page_table(){}

frame_id page_table::query(file_id fid,page_id pid)const
{
    return query({fid,pid});
}

frame_id page_table::query(const page_key & pk)const
{
    auto it = hash_table.find(pk);
    if(it == hash_table.end())return invalid_frame_id;
    return it->second;
}

bool page_table::contains(file_id fid,page_id pid)const
{
    return contains({fid,pid});
}

bool page_table::contains(const page_key & pk)const
{
    auto it = hash_table.find(pk);
    return it != hash_table.end();
}

bool page_table::insert(file_id fid,page_id pid,frame_id frid)
{
    return insert({fid,pid},frid);
}

bool page_table::insert(const page_key & pk,frame_id fid)
{
    return hash_table.emplace(pk, fid).second;
}


bool page_table::erase(file_id fid,page_id pid)
{
    return erase({fid,pid});
}


bool page_table::erase(const page_key & k)
{
    return hash_table.erase(k) != 0;
}


void page_table::clear()noexcept
{
    hash_table.clear();
}

bool page_table::empty()const noexcept
{
    return hash_table.empty();
}


std::size_t page_table::size()const noexcept
{
    return hash_table.size();
}

void page_table::reserve(std::size_t cap)
{
    hash_table.reserve(cap);
}



/*

Class: FREE LIST

Docs:
    Manages free frame IDs for allocation.

Class Methods:
    empty()                 -> whether the free list is empty
    size()                  -> number of available frames
    contains(frame_id fid)  -> whether the frame is free; false if OOR
    get_empty_frame()       -> allocate a free frame; invalid_frame_id if empty
    push(frame_id fid)      -> return a used frame; false if OOR or already free
    clear()                 -> restore every frame to the free list

*/

free_list::free_list(std::size_t cap):next(checked_frame_count(cap)),linked(cap,true),head(0),count(cap)
{
    for(std::size_t i = 0;i < cap - 1;++i)
        next[i] = i+1;
    next[cap-1] = invalid_frame_id;
}

bool free_list::empty()const noexcept
{
    return count == 0;
}

std::size_t free_list::size()const noexcept
{
    return count;
}


bool free_list::contains(frame_id fid)const noexcept
{
    if(fid >= next.size()) return false;

    return linked[fid];
}


frame_id free_list::get_empty_frame()noexcept
{
    if(head == invalid_frame_id)
    return head;

    frame_id x = head;
    linked[head] = false;
    head = next[head];
    --count;
    return x;
}

bool free_list::push(frame_id fid)noexcept
{
    if(fid >= next.size()) return false;
    if(linked[fid])return false;
    linked[fid] = true;
    next[fid] = head;
    head = fid;
    ++count;
    return true;
}

void free_list::clear()noexcept
{
    for(std::size_t i = 0;i < next.size();++i)
    {
        next[i] = i+1;
        linked[i] = true;
    }
    next[next.size()-1] = invalid_frame_id;

    head = 0;
    count = next.size();
}



/*

Class: _LIST

Docs:
    A fixed-capacity, array-based doubly linked list for an LRU cache.
    Stores frame IDs and does not own the frames themselves.

Class Methods:
    empty()                  -> Whether the list is empty.
    size()                   -> Number of frames currently in the list.
    contains(frame_id fid)   -> Whether the list contains this frame.
    push_front(frame_id id)  -> Insert a frame at the front if it is not
                                 already in the list.
    move_front(frame_id id)  -> Move an existing frame to the front.
    erase(frame_id fid)      -> Remove a frame from the list.
    pop_back()               -> Remove and return the last frame.
                                 Returns invalid_frame_id if the list is empty.
    clear()                  -> remove all entries
*/

_List::_List(std::size_t cap) : prev(std::vector<frame_id>(cap,invalid_frame_id)),
                                next(std::vector<frame_id>(cap,invalid_frame_id)),
                                linked(std::vector<bool>(cap,false)),
                                head(invalid_frame_id),
                                tail(invalid_frame_id),
                                count(0)
{

}

bool _List::empty()const noexcept
{
    return count == 0;
}

std::size_t _List::size()const noexcept
{
    return count;
}

bool _List::contains(frame_id fid)const noexcept
{
    if(fid >= next.size()) return false;
    return linked[fid];
}

bool _List::push_front(frame_id id)noexcept
{
    if(id >= next.size() || linked[id]) return false;

    prev[id] = invalid_frame_id;
    next[id] = head;
    if(head != invalid_frame_id)
    {
        next[id] = head;
        prev[head] = id;
        head = id;
    }
    else
    {
        head = id;
        tail = id;
    }
    ++count;
    linked[id] = true;
    return true;
}

bool _List::move_front(frame_id id)noexcept
{
    if(!contains(id)) return false;
    if(head == id) return true;
    return erase(id) && push_front(id);

}


bool _List::erase(frame_id fid)noexcept
{
    if(!contains(fid)) return false;


    uint8_t mask = (next[fid] != invalid_frame_id) | (prev[fid] != invalid_frame_id) << 1;
    switch(mask)
    {
    case 0b00:{
        head = invalid_frame_id;
        tail = invalid_frame_id;
    };break;
    case 0b01:{
        head = next[fid];
        prev[head] = invalid_frame_id;
        next[fid] = invalid_frame_id;
    };break;
    case 0b10:{
        tail = prev[fid];
        next[tail] = invalid_frame_id;
        prev[fid] = invalid_frame_id;
    };break;
    default:{
        frame_id _prev = prev[fid];
        frame_id _next = next[fid];
        next[_prev] = _next;
        prev[_next] = _prev;
        next[fid] = prev[fid] = invalid_frame_id;
    }
    }
    linked[fid] = false;
    --count;
    return true;
}

frame_id _List::pop_back()noexcept
{
    if(count == 0)return invalid_frame_id;
    frame_id popped = tail;
    erase(popped);
    return popped;
}


void _List::clear()noexcept
{
    head = tail = invalid_frame_id;
    count = 0;
    for(std::size_t i = 0;i < next.size();++i)
    {
        prev[i] = next[i] = invalid_frame_id;
        linked[i] = false;
    }
}






/*

Class: LRU LIST

Docs:
    A lightweight wrapper around _List for LRU cache management.
    Maintains access order and tracks pinned frames.
    The front stores the most recently accessed frame, while the
    back stores the least recently accessed frame.

Class Methods:
    empty()                    -> whether the list is empty
    size()                     -> number of frames currently in the list
    contains(frame_id fid)     -> whether the frame is in the list
    pin(frame_id fid)          -> mark the frame as pinned
    unpin(frame_id fid)        -> mark the frame as unpinned
    record_access(frame_id fid)-> move the frame to the front; false if OOR
    victim()                   -> remove and return the least recently used
                                  unpinned frame; invalid_frame_id if none
    clear()                    -> remove all entries and pin flags

*/



LRU_list::LRU_list(std::size_t cap):list(cap),_cap(cap),pinned(std::vector<bool>(cap,false))
{

}


bool LRU_list::empty()const noexcept
{
    return list.empty();
}

std::size_t LRU_list::size()const noexcept
{
    return list.size();
}

void LRU_list::pin(frame_id fid)noexcept
{
    if(fid >= _cap) return;
    if(!pinned[fid])pinned[fid] = true;
}

void LRU_list::unpin(frame_id fid)noexcept
{
    if(fid >= _cap) return;
    if(pinned[fid])pinned[fid] = false;
}

bool LRU_list::contains(frame_id fid)const noexcept
{
    return list.contains(fid);
}


frame_id LRU_list::victim(bool remove,frame_id fid)noexcept
{
    if(fid != invalid_frame_id)
    {
        if (!list.contains(fid) || pinned[fid])
            return invalid_frame_id;
        if (remove)
            list.erase(fid);
        return fid;
    }
    frame_id candidate = list.tail;
    while(candidate != invalid_frame_id)
    {
        if(!pinned[candidate])
            break;
        candidate = list.prev[candidate];
    }

    if(remove)
        list.erase(candidate);

    return candidate;
}


void LRU_list::clear()noexcept
{
    list.clear();
    for(std::size_t i = 0;i < pinned.size();++i)
        pinned[i] = false;
}

bool  LRU_list::record_access(frame_id fid)noexcept
{
    if (fid >= _cap)
        return false;

    if (list.contains(fid))
        return list.move_front(fid);

    return list.push_front(fid);
}






/*

Class: PAGE GUARD

Docs:
    A move-only RAII handle for managing page access in the buffer pool.
    Keeps the associated frame pinned during its lifetime and automatically
    unpins it upon release or destruction.
    Provides read-only or writable access to the underlying page.

Class Methods:
    ~page_guard()                  -> release the held frame automatically
    page_guard(page_guard&&)       -> transfer ownership from another guard
    operator=(page_guard&&)        -> release the current frame and take
                                      ownership from another guard
    view()                         -> get a const pointer to the page;
                                      nullptr if the guard is invalid
    get()                          -> get a mutable pointer to the page;
                                      nullptr if invalid or not writable
    release()                      -> unpin the frame and invalidate the guard
    frame()                        -> get the associated frame ID
    file()                         -> get the ID of the file containing the page
    page_no()                      -> get the page ID within the file
    writable()                     -> whether the guard permits write access
    operator bool()                -> whether the guard is valid

*/


page_guard::page_guard(frame_id fid,buffer_pool_manager* bpm_,bool can_write_)noexcept:f(fid),bpm(bpm_),can_write(can_write_)
{
    bpm->pin(f);
}

page_guard::page_guard(page_guard && ppp)noexcept
{
    f = ppp.f;
    bpm = ppp.bpm;
    can_write = ppp.can_write;

    ppp.f = invalid_frame_id;
    ppp.bpm = nullptr;
    ppp.can_write = false;
}

page_guard &page_guard::operator=(page_guard && ppp)noexcept
{
    if (this == &ppp)
        return *this;

    release();

    f = ppp.f;
    bpm = ppp.bpm;
    can_write = ppp.can_write;

    ppp.f = invalid_frame_id;
    ppp.bpm = nullptr;
    ppp.can_write = false;

    return *this;
}


page_guard::~page_guard()noexcept
{
    release();
}


page_guard::operator bool()const noexcept
{
    return bpm != nullptr && f != invalid_frame_id;
}

void page_guard::release()noexcept
{
    bool released =  !this->operator bool();
    if(released)
        return;
    buffer_pool_manager * tmp = bpm;
    frame_id tmp_f = f;
    bpm = nullptr;
    f = invalid_frame_id;
    can_write = false;

    tmp->unpin(tmp_f);
}


const page * page_guard::view()const noexcept
{
    if(!bool(*this)) return nullptr;
    const db::frame * fr = bpm->get_frame(f);
    if(fr == nullptr)return nullptr;
    return &(fr->_page);
}


page* page_guard::get()noexcept
{
    if(!bool(*this) || !can_write) return nullptr;
    db::frame * fr = bpm->get_frame(f);
    if(fr == nullptr) return nullptr;

    bpm->mark_dirty(f);
    return &(fr->_page);
}


frame_id page_guard::frame()const noexcept
{
    return f;
}


file_id page_guard::file()const noexcept
{
    if (!bool(*this))
        return 0;
    const db::frame * fr = bpm->get_frame(f);
    return fr ? fr->fid : 0;
}

page_id page_guard::page_no()const noexcept
{
    if (!bool(*this))
        return 0;
    const db::frame * fr = bpm->get_frame(f);
    return fr ? fr->pid : 0;
}


bool page_guard::writable()const noexcept
{
    return can_write;
}


/*

Class : BUFFER POOL MANAGER

Docs:
    Single-threaded page cache. The page table and per-file page sets describe
    the same resident frames. See docs/BUFFER_POOL.zh-CN.md for lifecycle,
    error handling, statistics and examples.
*/

frame * buffer_pool_manager::get_frame(frame_id fid)noexcept
{
    if(fid >= frames.size()) return nullptr;
    return &frames[fid];
}

const frame * buffer_pool_manager::get_frame(frame_id fid)const noexcept
{
    if(fid >= frames.size()) return nullptr;
    return &frames[fid];
}

frame_id buffer_pool_manager::find_frame(file_id fid,page_id pid)const
{
    return table.query(fid,pid);
}


frame_id buffer_pool_manager::acquire_frame()
{
    //尝试利用free list中的空页
    frame_id candidate = free_frames.get_empty_frame();
    if(candidate != invalid_frame_id)
        return candidate;

    //没有空闲的话，在lru cache中找一个
    candidate = lru.victim(false);
    if(candidate == invalid_frame_id)
        return candidate;

    frame * fr = get_frame(candidate);
    if(!fr)
        return invalid_frame_id;
    //如果脏了的话就写回去
    if(fr->dirty)
        write_back(*fr);

    remove_mapping(candidate);
    lru.victim(true,candidate);
    fr->reset();

    stat.evictions += 1;
    return candidate;
}


void buffer_pool_manager::pin(frame_id fid)noexcept
{
    frame * fr = get_frame(fid);
    if(fr == nullptr)
        return;

    if(fr->pin_count == 0)
        lru.pin(fid);

    fr->pin_count += 1;
}


void buffer_pool_manager::unpin(frame_id fid)noexcept
{
    frame * fr = get_frame(fid);
    if(fr == nullptr)
        return;
    if(fr->pin_count <= 0) return;

    fr->pin_count -= 1;
    if(fr->pin_count == 0)
        lru.unpin(fid);
}


void buffer_pool_manager::mark_dirty(frame_id fid)noexcept
{
    frame * fr = get_frame(fid);
    if(fr == nullptr)
        return;

    if(!fr->dirty)
        fr->dirty = true;
}


void buffer_pool_manager::write_back(frame & fr)
{
    if (!fr.valid || !fr.dirty)
        return;

    file_id fid = fr.fid;
    page_id pid = fr.pid;
    db::file_handle & fh = fm.get_file(fid);
    fh.write_page(pid,fr._page);
    // A guard may still modify a previously returned mutable pointer.
    fr.dirty = (fr.pin_count != 0);
    stat.writebacks += 1;
}



void buffer_pool_manager::add_mapping(frame_id fid)
{
    const db::frame & fr = frames[fid];
    auto result = file_pages.try_emplace(fr.fid);
    auto file_it = result.first;

    try {
        if (!file_it->second.insert(fr.pid).second)
            throw pool_error(error_code::internal_error, "duplicate file-page mapping",
                             "buffer_pool.add_mapping", fr.fid, fr.pid);
    }
    catch (...) {
        if (result.second && file_it->second.empty())
            file_pages.erase(file_it);
        throw;
    }

    try {
        if (!table.insert(fr.fid, fr.pid, fid))
            throw pool_error(error_code::internal_error, "duplicate page mapping",
                             "buffer_pool.add_mapping", fr.fid, fr.pid);
    }
    catch (...) {
        file_it->second.erase(fr.pid);
        if (file_it->second.empty())
            file_pages.erase(file_it);
        throw;
    }
}

void buffer_pool_manager::remove_mapping(frame_id fid)
{
    frame * fr = get_frame(fid);
    if(fr == nullptr)
        return;

    file_id _fid = fr->fid;
    page_id pid = fr->pid;

    table.erase(_fid,pid);
    auto file_it = file_pages.find(_fid);
    if (file_it != file_pages.end()) {
        file_it->second.erase(pid);
        if (file_it->second.empty())
            file_pages.erase(file_it);
    }
}


buffer_pool_manager::buffer_pool_manager(file_manager & fm_,std::size_t frame_count) :  fm(fm_),
                                                                                        frames(checked_frame_count(frame_count)),
                                                                                        table(),
                                                                                        free_frames(frame_count),
                                                                                        lru(frame_count)
{
    table.reserve(frame_count);
}


page_guard buffer_pool_manager::fetch_page(file_id fid,page_id pid,access_type access)
{
    require_data_page(fid, pid, "buffer_pool.fetch_page");
    if (access != access_type::read && access != access_type::write)
        throw pool_error(error_code::invalid_argument, "invalid page access mode",
                         "buffer_pool.fetch_page", fid, pid);
    db::file_handle & fh = fm.get_file(fid);
    if (!fh.is_open())
        throw pool_error(error_code::invalid_file_handle, "file handle is closed",
                         "buffer_pool.fetch_page", fid, pid);
    if (pid >= fh.get_num_pages())
        throw pool_error(error_code::invalid_page, "page ID is out of range",
                         "buffer_pool.fetch_page", fid, pid);
    //先从buffer pool之中寻找
    frame_id candidate = find_frame(fid,pid);
    bool can_write = (access == access_type::write);
    if(candidate != invalid_frame_id)
    {
        stat.hits += 1;
        lru.record_access(candidate);
        return page_guard(candidate,this,can_write);
    }

    stat.misses += 1;
    candidate = acquire_frame();
    if(candidate == invalid_frame_id)
        throw pool_error(error_code::buffer_exhausted, "buffer pool exhausted",
                         "buffer_pool.fetch_page", fid, pid);

    frame * fr = get_frame(candidate);
    try{
        page & p = fr->_page;
        fh.read_page(pid,p);
        fr->set(fid,pid);
        add_mapping(candidate);
    }
    catch(...){
        if(fr)
            fr->reset();
        free_frames.push(candidate);
        throw;
    };


    lru.record_access(candidate);
    return page_guard(candidate,this,can_write);
}


page_guard buffer_pool_manager::new_page(file_id fid)
{
    db::file_handle & fh = fm.get_file(fid);
    if (!fh.is_open())
        throw pool_error(error_code::invalid_file_handle, "file handle is closed",
                         "buffer_pool.new_page", fid);
    frame_id id = acquire_frame();
    if(id == invalid_frame_id)
        throw pool_error(error_code::buffer_exhausted, "buffer pool exhausted",
                         "buffer_pool.new_page", fid);

    frame & _frame = frames[id];
    try{
        page_id pid = fh.allocate_page();
        _frame._page.fill(std::byte(0));
        _frame.set(fid,pid);
        add_mapping(id);
    }
    catch (...) {
        _frame.reset();
        free_frames.push(id);
        throw;
    }
    lru.record_access(id);
    return page_guard(id, this, true);
}


bool buffer_pool_manager::flush_page(file_id fid, page_id pid)
{
    require_data_page(fid, pid, "buffer_pool.flush_page");
    frame_id candidate = find_frame(fid,pid);
    if(candidate == invalid_frame_id)
        return false;

    db::frame & fr = frames[candidate];
    write_back(fr);
    return true;
}

std::size_t buffer_pool_manager::flush_file(file_id fid)
{
    auto file_it = file_pages.find(fid);
    if (file_it == file_pages.end())
        return 0;

    std::size_t count = 0;
    for (page_id pid : file_it->second) {
        db::frame & fr = frames[find_frame(fid, pid)];
        if (fr.dirty) {
            write_back(fr);
            ++count;
        }
    }

    return count;
}

std::size_t buffer_pool_manager::flush_all()
{
    std::size_t count = 0;
    for (db::frame & fr : frames) {
        if (fr.valid && fr.dirty) {
            write_back(fr);
            ++count;
        }
    }
    return count;
}

bool buffer_pool_manager::evict_page(file_id fid, page_id pid)
{
    require_data_page(fid, pid, "buffer_pool.evict_page");
    frame_id id = find_frame(fid, pid);
    if (id == invalid_frame_id || frames[id].pin_count != 0)
        return false;

    db::frame & fr = frames[id];
    write_back(fr);
    remove_mapping(id);
    lru.victim(true, id);
    fr.reset();
    free_frames.push(id);
    ++stat.evictions;
    return true;
}

bool buffer_pool_manager::discard_page(file_id fid, page_id pid)
{
    require_data_page(fid, pid, "buffer_pool.discard_page");
    frame_id id = find_frame(fid, pid);
    if (id == invalid_frame_id || frames[id].pin_count != 0)
        return false;

    remove_mapping(id);
    lru.victim(true, id);
    frames[id].reset();
    free_frames.push(id);
    return true;
}

std::size_t buffer_pool_manager::flush_and_invalidate_file(file_id fid)
{
    auto file_it = file_pages.find(fid);
    if (file_it == file_pages.end())
        return 0;

    for (page_id pid : file_it->second) {
        if (frames[find_frame(fid, pid)].pin_count != 0)
            throw pool_error(error_code::resource_busy, "file still has pinned pages",
                             "buffer_pool.flush_and_invalidate_file", fid, pid);
    }

    // Keep every mapping until all writes have succeeded.
    flush_file(fid);
    const std::size_t count = file_it->second.size();
    for (std::size_t i = 0; i < count; ++i) {
        // Removing the last page erases its file bucket, so reacquire it.
        file_it = file_pages.find(fid);
        page_id pid = *file_it->second.begin();
        discard_page(fid, pid);
    }
    return count;
}

void buffer_pool_manager::close_file(file_id fid)
{
    // Validate before changing the cache, including calls on unknown handles.
    (void)fm.get_file(fid);
    flush_and_invalidate_file(fid);
    fm.close_file(fid);
}

void buffer_pool_manager::shutdown()
{
    for (const db::frame & fr : frames) {
        if (fr.pin_count != 0)
            throw pool_error(error_code::resource_busy, "buffer pool still has pinned pages",
                             "buffer_pool.shutdown", fr.fid, fr.pid);
    }
    flush_all();
    table.clear();
    file_pages.clear();
    lru.clear();
    for (db::frame & fr : frames)
        fr.reset();
    free_frames.clear();
}

buffer_pool_manager::~buffer_pool_manager() noexcept
{
    for (db::frame & fr : frames) {
        try {
            write_back(fr);
        }
        catch (...) {
            // Report write errors through explicit shutdown(), not destruction.
        }
    }
}

std::size_t buffer_pool_manager::frame_count() const noexcept
{
    return frames.size();
}

std::size_t buffer_pool_manager::free_frame_count() const noexcept
{
    return free_frames.size();
}

std::size_t buffer_pool_manager::cached_page_count() const noexcept
{
    return table.size();
}

const buffer_pool_stats & buffer_pool_manager::stats() const noexcept
{
    return stat;
}

void buffer_pool_manager::check_invariants() const
{
    auto require = [](bool ok, const char * message) {
        if (!ok)
            throw pool_error(error_code::internal_error, message, "buffer_pool.check_invariants");
    };

    const std::size_t capacity = frames.size();
    std::size_t resident = 0;
    for (std::size_t i = 0; i < capacity; ++i) {
        frame_id id = static_cast<frame_id>(i);
        const db::frame & fr = frames[i];
        require(lru.pinned[i] == (fr.pin_count != 0), "pin state mismatch");
        if (fr.valid) {
            ++resident;
            require(!free_frames.contains(id), "valid frame is free");
            require(lru.contains(id), "valid frame missing from LRU");
            require(table.query(fr.fid, fr.pid) == id, "frame mapping mismatch");
            auto file_it = file_pages.find(fr.fid);
            require(file_it != file_pages.end() && file_it->second.count(fr.pid) == 1,
                    "frame missing from file index");
        }
        else {
            require(free_frames.contains(id), "invalid frame is not free");
            require(!lru.contains(id), "invalid frame is in LRU");
            require(fr.pin_count == 0 && !fr.dirty, "invalid frame has active state");
        }
    }
    require(table.size() == resident, "page table size mismatch");
    require(lru.size() == resident, "LRU size mismatch");
    require(free_frames.size() + resident == capacity, "frame count mismatch");

    std::size_t indexed = 0;
    for (const auto & file_entry : file_pages) {
        require(!file_entry.second.empty(), "empty file index bucket");
        for (page_id pid : file_entry.second) {
            frame_id id = table.query(file_entry.first, pid);
            require(id < capacity, "file index has no page mapping");
            const db::frame & fr = frames[id];
            require(fr.valid && fr.fid == file_entry.first && fr.pid == pid,
                    "file index identity mismatch");
            ++indexed;
        }
    }
    require(indexed == resident, "file index size mismatch");
    for (const auto & entry : table.hash_table) {
        require(entry.second < capacity, "mapping frame ID out of range");
        const db::frame & fr = frames[entry.second];
        require(fr.valid && fr.fid == entry.first.fid && fr.pid == entry.first.pid,
                "page table identity mismatch");
    }

    // Validate links as well as membership bits; bound walks to detect cycles.
    std::vector<bool> seen(capacity, false);
    std::size_t count = 0;
    frame_id id = free_frames.head;
    while (id != invalid_frame_id) {
        require(id < capacity && !seen[id], "invalid free list link or cycle");
        require(free_frames.linked[id], "free list membership mismatch");
        seen[id] = true;
        ++count;
        id = free_frames.next[id];
    }
    require(count == free_frames.size(), "free list link count mismatch");
    for (std::size_t i = 0; i < capacity; ++i)
        require(seen[i] == free_frames.linked[i], "unreachable free frame");

    std::fill(seen.begin(), seen.end(), false);
    count = 0;
    frame_id previous = invalid_frame_id;
    id = lru.list.head;
    while (id != invalid_frame_id) {
        require(id < capacity && !seen[id], "invalid LRU link or cycle");
        require(lru.list.linked[id] && lru.list.prev[id] == previous,
                "LRU link mismatch");
        seen[id] = true;
        ++count;
        previous = id;
        id = lru.list.next[id];
    }
    require(previous == lru.list.tail, "LRU tail mismatch");
    require(count == lru.size(), "LRU link count mismatch");
    for (std::size_t i = 0; i < capacity; ++i)
        require(seen[i] == lru.list.linked[i], "unreachable LRU frame");
}

}
