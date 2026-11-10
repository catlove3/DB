#include "db/storage/buffer_pool.h"
#include "db/common/types.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace db
{

namespace {

/*
    HELPERS : BUFFER POOL VALIDATION
    docs:
        Validate arguments or construct owning diagnostics before changing cache state.

    methods:
        pool_error          : Return an error value with copied message/operation and optional file/page IDs.
                              The caller throws it; valid string pointers are required.
        require_data_page   : Reject page 0 with invalid_page; leave other page/file checks to the caller.
        checked_frame_count : Require a count in [1,invalid_frame_id], then return it before allocation.

    errors:
        Invalid capacity -> invalid_argument. Error construction can propagate allocation failures.
        These helpers do not access files, update counters or mutate existing cache state.
*/
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

/*
    STRUCTS : PAGE KEY AND FRAME
    docs:
        page_key identifies a cached page by both file ID and page ID.
        hash_page_key mixes those IDs; collisions are permitted and hashes are never persisted.
        frame owns one 8192-byte page buffer plus identity, pin_count, valid and dirty fields.

    methods:
        page_key::operator== : Compare both IDs without consulting file registration.
        frame::reset         : Reset metadata without clearing the page bytes or performing I/O.
        frame::set           : Assign metadata without validating it or updating pool indexes/LRU/free list.
*/
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
    CLASS : PAGE TABLE
    docs:
        Map (file_id,page_id) to frame_id using a hash table; own neither pages nor frames.
        Overloads taking separate IDs have the same behavior as the page_key overloads.

    methods:
        constructor : Create an empty mapping.
        query       : Return the frame ID, or invalid_frame_id when absent.
        contains    : Test key membership without validating the file or resident frame.
        insert      : Add a new mapping; return false without overwriting an existing key.
        erase       : Remove only the mapping; return false when absent, without releasing a frame.
        clear       : Remove all mappings without touching pages or files.
        empty/size  : Report membership count, not allocated capacity or bucket count.
        reserve     : Reserve buckets for at least cap entries without inserting any mapping.

    errors:
        Callers supply valid frame IDs and associations; this class does not check frame uniqueness.
        Allocation failures propagate; failed single insertions preserve existing mappings.
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
    CLASS : FREE LIST
    docs:
        Track free frame IDs with array-based links. Own no page buffers.
        Initially every frame in [0,cap) is free and allocation order is increasing.

    methods:
        constructor     : Initialize all links and membership bits.
        empty/size      : Report the number of frames currently available.
        contains        : Check whether an in-range frame ID is free; out-of-range returns false.
        get_empty_frame : Remove the head; return invalid_frame_id without changes when empty.
        push            : Return a frame to the head; reject out-of-range/already-free IDs with false.
        clear           : Restore all IDs in increasing order without clearing actual frame bytes.

    errors:
        Capacity 0 or greater than invalid_frame_id -> invalid_argument; allocation failures propagate.
        Callers must release actual frame ownership before push/clear; list operations perform no I/O.
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
    CLASS : _LIST
    docs:
        A fixed-capacity doubly linked list whose members are frame IDs, stored in parallel arrays.
        Own neither frame data nor pin counts. Front/back represent most/least recent positions.

    methods:
        constructor : Start empty; cap may be zero and must not exceed invalid_frame_id.
        empty/size  : Report membership, not capacity.
        contains    : Return false for absent or out-of-range IDs.
        push_front  : Add an absent in-range ID; otherwise return false without changes.
        move_front  : Move an existing member; return true even when already at the front.
        erase       : Unlink a member; absent/out-of-range IDs return false.
        pop_back    : Remove the tail, or return invalid_frame_id for an empty list.
        clear       : Reset links/membership while retaining capacity and external frame contents.

    errors:
        Constructor allocation failures propagate; capacity is a caller precondition.
        Operations after construction do not allocate and are noexcept.
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
    CLASS : LRU LIST
    docs:
        Wrap _List with one pinned flag per frame. The front is most recently accessed.
        Pin flags are eligibility bits, not reference counts; the buffer pool maintains the counts.
        Membership includes pinned frames, so a nonempty list can still have no eviction candidate.

    methods:
        constructor   : Start empty with all flags clear; cap may be zero, at most invalid_frame_id.
        empty/size    : Report all members, including pinned ones.
        contains      : Check membership independently of pin state; out-of-range returns false.
        record_access : Insert or move an in-range ID to the front without changing its pin flag.
        victim        : With a supplied ID, inspect only it; otherwise scan backward from the tail.
                        Return invalid_frame_id when absent/pinned or no eligible member exists.
                        remove=false only inspects; remove=true unlinks the selected member.
        pin/unpin     : Set/clear an in-range flag; ignore out-of-range IDs and do not count calls.
        clear         : Remove all members and clear all pin flags without touching page data.

    errors:
        Constructor allocation failures propagate; post-construction methods do not allocate or do I/O.
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
    CLASS : PAGE GUARD
    docs:
        A move-only RAII borrow of one resident frame, holding exactly one pin until released.
        Own neither the pool nor the file. The pool must outlive every guard and borrowed page pointer.
        Callers must not directly close the file while a guard exists. This is not a lock or transaction.

    methods:
        constructor     : Pool-only construction; pin a valid resident frame without allocation or I/O.
        move constructor: Transfer the existing pin/access mode; leave the source empty and read-only.
        move assignment : Release this pin, then transfer the source; self-move changes nothing.
        destructor      : Release the pin without flushing, clearing dirty state or reporting I/O errors.
        view            : Borrow a const page pointer; empty guards return nullptr without marking dirty.
        get             : Borrow a mutable pointer only when valid and writable; immediately mark dirty.
        release         : Unpin once and become empty; repeated release is harmless.
        frame           : Return the frame ID, or invalid_frame_id when empty.
        file/page_no    : Return the page identity, or 0 when empty; file ID 0 is also a valid ID.
        writable        : Report write permission; permission alone does not mark the page dirty.
        operator bool   : Check the stored pool pointer/frame sentinel, without probing OS state.

    errors:
        Copying is disabled. Page pointers expire when their pin is released; moving transfers that lifetime.
        Empty/read-only get() returns nullptr. Pool-only construction requires a valid, non-null pool.
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
    CLASS : BUFFER POOL MANAGER
    docs:
        Own a fixed set of page frames in one thread; cache only data pages with pid >= 1.
        Borrow file_manager, which must outlive this pool; all guards must be released before destruction.
        Copying/moving is disabled so guard pointers keep referring to the same pool.
        Page table and per-file page sets describe exactly the same resident frames.
        Registered cached files must use this pool for page access and coordinated closure.

    methods:
        constructor : Allocate [1,invalid_frame_id] frames, initially free with zero statistics; no I/O.
        destructor  : Best-effort flush each dirty frame, ignoring errors; do not close files.
        fetch_page  : Validate file/page/access, record hit/miss, and return a guard with one new pin.
                      Read guards permit view only; write guards mark dirty only when get() is called.
        new_page    : Acquire a frame before appending a zero page; return a writable, initially clean guard.
                      Do not count hits/misses. No available frame means no disk allocation.
        flush_page  : Return whether a page is resident, even when clean; pinned pages may be written.
        flush_file  : Write this file's resident dirty pages; return successful writes, zero when none reside.
        flush_all   : Write valid dirty frames in frame order; return the number actually written.
        evict_page  : Write and remove an unpinned page, return its frame and increment evictions.
                      Missing/pinned pages return false without a write; disk pages are not deleted.
        discard_page: Drop an unpinned page without writing or changing statistics; caller accepts data loss.
                      Missing/pinned pages return false; disk pages remain allocated.
        flush_and_invalidate_file : Reject any pin first, flush all dirty pages, then remove all mappings.
                                    Return removed resident count, not write count; do not count evictions.
        close_file  : Validate registration, flush/invalidate, then close/unregister through file_manager.
        shutdown    : Reject any pin before writing; flush, then reset the cache without closing files.
                      Statistics remain, repeated shutdown is safe, and the pool can be reused.
        frame_count/free_frame_count/cached_page_count : Report capacity, free frames and resident mappings.
        stats       : Borrow live counters; copy explicitly for a snapshot. Hits/misses count valid fetches,
                      evictions count automatic/explicit eviction, writebacks count successful dirty writes.
        check_invariants : Check mappings, frames, free list, LRU and pins; do not repair or perform I/O.

    helpers:
        get_frame   : Borrow an in-range frame; out-of-range returns nullptr without pinning/validation.
        find_frame  : Return a mapping or invalid_frame_id, without checking file lifecycle/page bounds.
        acquire_frame : Take a free frame or write/evict an unpinned LRU victim; return invalid_frame_id
                        if all candidates are pinned. Caller must register the frame or return it on failure.
        pin/unpin   : Maintain pin_count and LRU eligibility at transitions 0/1; ignore invalid IDs.
                      pin requires a valid resident frame and a non-overflowing counter; unpin ignores zero.
        mark_dirty  : Mark an in-range resident frame; ignore out-of-range IDs.
        write_back  : Write valid dirty frames; keep dirty while any pin remains, even after a successful write.
        add_mapping : Add page/per-file entries together, rolling back new entries if insertion fails.
        remove_mapping : Remove both entries and empty per-file sets; do not reset the frame or LRU.

    errors:
        Bad capacity/access -> invalid_argument; page 0/out-of-range page -> invalid_page;
        unknown/closed file -> invalid_file_handle; no available frame -> buffer_exhausted;
        active guards blocking closure/shutdown -> resource_busy; broken invariants -> internal_error.
        File-layer errors and allocation failures propagate. Invalid fetch arguments do not change counters;
        a valid miss is counted even if later I/O fails. Failed read/mapping returns its acquired frame.
        Dirty write failure preserves mappings and dirty state; disk bytes may already be partially written.
        Successful earlier writes/evictions/counters are not rolled back after a later failure.
        Successful disk allocation followed by mapping failure leaves the new disk page allocated.
        Invalidation removes mappings only after all writes succeed; earlier writes may already clear dirty.
        After invalidation, header-save failure leaves the file registered; once the close syscall runs,
        even close failure unregisters it. Repeated close_file reports invalid_file_handle.
        Missing cached pages need not validate their file handles. flush_file traverses an unspecified order.
        Invariant checks may allocate and do not validate file liveness or page contents.
        Flush completion does not provide fsync, transaction rollback or power-loss recovery.
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
    // Prefer a free frame before selecting an eviction candidate.
    frame_id candidate = free_frames.get_empty_frame();
    if(candidate != invalid_frame_id)
        return candidate;

    // Inspect the LRU candidate without unlinking it yet.
    candidate = lru.victim(false);
    if(candidate == invalid_frame_id)
        return candidate;

    frame * fr = get_frame(candidate);
    if(!fr)
        return invalid_frame_id;
    // Preserve the old mapping if the dirty write fails.
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
    // Check whether the page is already resident.
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
    // Report a failed invariant without repairing state or changing counters.
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
