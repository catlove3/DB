#include <db/record/record.h>
#include <db/storage/file_manager.h>
#include <db/storage/buffer_pool.h>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace db
{

namespace{
    constexpr std::size_t header_bytes = 64;
    constexpr std::size_t ver_offset = 4;
    constexpr std::size_t record_size_offset = 8;
    constexpr std::size_t slot_count_offset = 12;
    constexpr std::size_t live_count_offset = 16;
    constexpr std::size_t flag_offset = 20;
    constexpr std::size_t next_free_offset = 24;
    constexpr std::size_t reserved_offset = 32;
    constexpr std::uint32_t record_magic = 0x31504d52; // RMP1
    constexpr std::uint32_t record_file_magic = 0x31464d52; // RMF1
    constexpr std::uint32_t record_ver = 1;
    constexpr std::size_t max_record_size = page_size - header_bytes - 1;
    constexpr std::size_t meta_free_head_offset = 16;
    constexpr std::size_t meta_total_records_offset = 24;
}


/*
    CLASS : RECORD PAGE HELPER
    docs:
        Manage fixed-size records within one in-memory page.
        File I/O, buffer guards, field decoding and free-page links belong to other layers.

    layout:
        [64-byte header][occupied bitmap][fixed-size record slots]
        R = record_size in bytes, valid range: 1..8127.
        S = largest num_slots satisfying 64 + ceil(S/8) + S*R <= page_size.
        B = (S + 7)/8; the payload offset of slot is 64 + B + slot*R.
        Slots start at 0; each bitmap byte maps slots from its lowest bit upward.
        A set bit means occupied. Header integers use little-endian encoding.
        The page magic is RMP1 and the format version is 1.
        live_count equals the number of set bits for valid slots.
        Reserved header bytes and unused bitmap bits must be zero.
        The file layer maintains flags and next_free; page CRUD leaves them unchanged.

    methods:
        clear        : Zero the entire page; the result still needs init to be valid.
        capacity     : Compute slot capacity; invalid sizes raise invalid_argument.
        init         : Validate the size, then clear the page and write an empty header.
        validate     : Check format, expected size, capacity, bitmap and count without writes.
        record_count : Validate the page and return its live record count.
        occupied     : Check a slot's occupancy bit; a valid empty slot returns false.
        first_free   : Return the lowest free slot or nullopt if full; slot 0 is valid.
        read         : Return an independent byte copy without borrowing page memory.
        insert       : Fill the lowest free slot, set its bit and increment the count.
                       Return nullopt if the page is full.
        update       : Replace the payload without changing the slot, bitmap or count.
        erase        : Lazy remove: clear the occupancy bit and decrement the count.
                       Keep the old payload; read rejects it and insert overwrites it on reuse.

    helpers:
        check_page           : Check the stored record size, then call validate.
        assess               : Check the slot range and expected occupancy.
        get_offset_and_resid : Compute the occupancy byte offset and bit index.
        get_record_offset    : Compute the record payload's byte offset within the page.
        find_free            : Find a free slot without repeating full page validation.
        All helpers except check_page require an already validated page.
        get_record_offset also requires a valid slot number.

    errors:
        Corrupt page format -> invalid_format; out-of-range slot -> invalid_rid.
        read/update/erase on a valid empty slot -> record_not_found.
        insert/update with a payload length different from R -> invalid_argument.
        All rejection checks precede writes; rejected operations leave page bytes unchanged.
*/

void record_page_helper::clear(page & p)
{
    p.fill(std::byte{0});
}


slot_id record_page_helper::capacity(slot_id record_size)
{
    if(record_size == 0 || record_size > max_record_size)
        throw db_error(
            error_code::invalid_argument,
            "record size must fit in a page"
        );

    constexpr std::size_t available_bytes = page_size - header_bytes;
    std::size_t slots = available_bytes / record_size;
    while(slots > 0)
    {
        const std::size_t bitmap_bytes = (slots + 7)/8;
        if(bitmap_bytes + slots * record_size <= available_bytes)
            return static_cast<slot_id>(slots);
        --slots;
    }
    throw db_error(
        error_code::invalid_argument,
        "unable to find fixed capacity for record to fit in a page"
    );
}


void record_page_helper::init(page& p, slot_id record_size)
{
    const slot_id num_slots = capacity(record_size);
    clear(p);
    /*
    写入page header:
        magic               ->uint32_t
        version             ->uint32_t
        record_size         ->uint32_t , measured in bytes
        num_slots           ->uint32_t
        num_valid_record    ->uint32_t
        flags               ->uint32_t , representing free list membership
        next_free           ->uint64_t , index of the next free page in free list
    */
    std::byte * dest = p.data();
    write_u32(dest,record_magic);
    write_u32(dest+ver_offset,record_ver);
    write_u32(dest+record_size_offset,record_size);
    write_u32(dest+slot_count_offset,num_slots);
    write_u32(dest+live_count_offset,0);
    write_u32(dest+flag_offset,0);
    write_u64(dest+next_free_offset,invalid_page_id);
}


void record_page_helper::validate(const page& p,std::uint32_t expected_record_size)
{
    // 参数错误和页内格式错误分别报告。
    const slot_id expected_slots = capacity(expected_record_size);
    const std::byte * src = p.data();
    const std::uint32_t magic = read_u32(src);
    const std::uint32_t version = read_u32(src+ver_offset);
    if(magic != record_magic || version != record_ver)
        throw db_error(
            error_code::invalid_format,
            "external or legacy page"
        );

    const slot_id record_size = read_u32(src+record_size_offset);
    if(record_size != expected_record_size)
        throw db_error(
            error_code::invalid_format,
            "unmatched record size"
        );

    const slot_id num_slots = read_u32(src+slot_count_offset);
    if(num_slots != expected_slots)
        throw db_error(
            error_code::invalid_format,
            "mismatched record_size and num_slots"
        );

    const slot_id live_count = read_u32(src+live_count_offset);
    if(live_count > num_slots)
        throw db_error(
            error_code::invalid_format,
            "live count exceeds num_slots"
        );

    const std::uint32_t flag = read_u32(src+flag_offset);
    if((flag & ~std::uint32_t{1}) != 0)
        throw db_error(
            error_code::invalid_format,
            "unknown flag bits in record page"
        );

    for(std::size_t i = reserved_offset;i < header_bytes;++i)
        if(p[i] != std::byte{0})
            throw db_error(
                error_code::invalid_format,
                "nonzero reserved bytes in record page"
            );

    const slot_id bitmap_bytes = (num_slots + 7)/8;
    const slot_id used_bits = num_slots % 8;
    if(used_bits != 0 &&
        (std::to_integer<unsigned>(p[header_bytes+bitmap_bytes-1]) >> used_bits) != 0)
        throw db_error(
            error_code::invalid_format,
            "nonzero unused bitmap bits"
        );

    // C++17 下逐字节统计，不调用会再次 validate 的公开 record_count。
    slot_id tot = 0;
    for(slot_id i = 0;i < bitmap_bytes;++i)
    {
        unsigned bits = std::to_integer<unsigned>(p[header_bytes+i]);
        while(bits != 0)
        {
            bits &= bits - 1;
            ++tot;
        }
    }
    if(tot != live_count)
        throw db_error(
            error_code::invalid_format,
            "live count mismatch with bitmap"
        );
}


void record_page_helper::check_page(const page & p)
{
    const slot_id record_size = read_u32(p.data()+record_size_offset);
    if(record_size == 0 || record_size > max_record_size)
        throw db_error(
            error_code::invalid_format,
            "invalid record size in page"
        );

    validate(p,record_size);
}


void record_page_helper::assess(const page & p,slot_id slot,bool should_exist)
{
    const slot_id num_slots = read_u32(p.data()+slot_count_offset);
    if(slot >= num_slots)
        throw db_error(
            error_code::invalid_rid,
            "slot id out of range"
        );

    slot_id byte_offset,bit_offset;
    get_offset_and_resid(slot,byte_offset,bit_offset);
    const unsigned bits = std::to_integer<unsigned>(p[byte_offset]);
    const bool occupy = (bits & (1u << bit_offset)) != 0;
    if(!occupy && should_exist)
        throw db_error(
            error_code::record_not_found,
            "record slot is empty"
        );

    if(occupy && !should_exist)
        throw db_error(
            error_code::invalid_format,
            "record slot is full but expect empty"
        );
}


void record_page_helper::get_offset_and_resid(slot_id slot,slot_id & byte_offset,slot_id& bit_offset) noexcept
{
    byte_offset = header_bytes + slot / 8;
    bit_offset = slot % 8;
}


std::size_t record_page_helper::get_record_offset(const page & p,slot_id slot) noexcept
{
    const slot_id num_slots = read_u32(p.data()+slot_count_offset);
    const slot_id record_size = read_u32(p.data()+record_size_offset);
    const std::size_t bitmap_bytes = (num_slots + 7)/8;
    return header_bytes + bitmap_bytes + std::size_t(slot) * record_size;
}


slot_id record_page_helper::record_count(const page& p)
{
    check_page(p);
    return read_u32(p.data()+live_count_offset);
}


bool record_page_helper::occupied(const page& p, slot_id slot)
{
    check_page(p);
    const slot_id num_slots = read_u32(p.data()+slot_count_offset);
    if(slot >= num_slots)
        throw db_error(
            error_code::invalid_rid,
            "slot id out of range"
        );

    slot_id byte_offset,bit_offset;
    get_offset_and_resid(slot,byte_offset,bit_offset);
    return (std::to_integer<unsigned>(p[byte_offset]) & (1u << bit_offset)) != 0;
}


std::optional<slot_id> record_page_helper::find_free(const page & p) noexcept
{
    const slot_id num_slots = read_u32(p.data()+slot_count_offset);
    const slot_id bitmap_bytes = (num_slots + 7)/8;
    for(slot_id i = 0;i < bitmap_bytes;++i)
    {
        const unsigned bits = std::to_integer<unsigned>(p[header_bytes+i]);
        if(bits == 0xff)
            continue;

        for(slot_id bit = 0;bit < 8 && i * 8 + bit < num_slots;++bit)
            if((bits & (1u << bit)) == 0)
                return i * 8 + bit;
    }
    return std::nullopt;
}


std::optional<slot_id> record_page_helper::first_free(const page& p)
{
    check_page(p);
    return find_free(p);
}


record_bytes record_page_helper::read(const page& p, slot_id slot)
{
    check_page(p);
    assess(p,slot,true);
    const slot_id record_size = read_u32(p.data()+record_size_offset);
    const std::size_t offset = get_record_offset(p,slot);

    record_bytes result(record_size);
    std::memcpy(result.data(),p.data()+offset,record_size);
    return result;
}


std::optional<slot_id> record_page_helper::insert(page& p,const record_bytes& bytes)
{
    check_page(p);
    const slot_id record_size = read_u32(p.data()+record_size_offset);
    if(bytes.size() != record_size)
        throw db_error(
            error_code::invalid_argument,
            "record length does not match"
        );

    const std::optional<slot_id> idx = find_free(p);
    if(!idx)
        return std::nullopt;

    slot_id byte_offset,bit_offset;
    get_offset_and_resid(*idx,byte_offset,bit_offset);
    const std::size_t offset = get_record_offset(p,*idx);
    const slot_id orig_live_count = read_u32(p.data()+live_count_offset);

    // 检查全部完成，下面只修改内存字节，不进行 I/O 或分配。
    std::memcpy(p.data()+offset,bytes.data(),record_size);
    p[byte_offset] |= static_cast<std::byte>(1u << bit_offset);
    write_u32(p.data()+live_count_offset,orig_live_count + 1);
    return idx;
}


void record_page_helper::update(page& p,slot_id slot,const record_bytes& bytes)
{
    check_page(p);
    const slot_id record_size = read_u32(p.data()+record_size_offset);
    if(bytes.size() != record_size)
        throw db_error(
            error_code::invalid_argument,
            "record length does not match"
        );

    assess(p,slot,true);
    const std::size_t offset = get_record_offset(p,slot);
    std::memcpy(p.data()+offset,bytes.data(),record_size);
}


void record_page_helper::erase(page& p, slot_id slot)
{
    check_page(p);
    assess(p,slot,true);
    slot_id byte_offset,bit_offset;
    get_offset_and_resid(slot,byte_offset,bit_offset);
    const slot_id orig_live = read_u32(p.data()+live_count_offset);

    p[byte_offset] &= ~static_cast<std::byte>(1u << bit_offset);
    write_u32(p.data()+live_count_offset,orig_live - 1);
}








/*
    CLASS : RECORD FILE
    docs:
        Manage fixed-size records across the data pages of one open file.
        Borrow fm, bpm and fid; the caller owns the managers and closes the file through bpm.
        The file must stay open and both managers must outlive this object.
        Use one active record_file per physical file; copying and moving are disabled.
        Cache only record_size_ and slots_per_page_; read mutable metadata from page 1.
        Keep guards local to each operation and return independent record byte copies.
        Field encoding, SQL execution and index maintenance belong to other layers.

    layout:
        page 0       : File header owned by file_manager; never accessed through bpm here.
        page 1       : RMF1 metadata, version 1, with little-endian integer fields.
        page 2..N-1  : RMP1 data pages managed by record_page_helper; N is the file page count.
        Metadata offsets and meta_data fields:
            0  : rm  -> uint32_t magic, RMF1 (0x31464d52).
            4  : v   -> uint32_t format version, 1.
            8  : rs  -> uint32_t record size R in bytes, valid range: 1..8127.
            12 : ns  -> uint32_t slots per data page S, equal to capacity(R).
            16 : nfp -> uint64_t free-page list head, or invalid_page_id for an empty list.
            24 : tr  -> uint64_t total live record count across all data pages.
        Bytes [32,page_size) are reserved and must be zero.
        A RID contains the data page number and slot number; it is relative to this file.
        Erased slots may be reused, so an old RID can later refer to a different record.
        nfp points to the first non-full page; each data page stores its own next_free link.
        Linked pages have flags bit 0 set; full pages are outside the free-page list.
        Insert uses the list head and removes it when full; a new non-full page becomes the head.
        Erase adds a page at the head only when that page was full before deletion.
        An initialized empty file has two pages; data pages are allocated on demand.

    methods:
        initialize   : Require a valid R, at least two buffer frames and a file with only page 0.
                       Append page 1 with nfp = invalid_page_id and tr = 0; do not allocate data pages.
        record_file  : Bind an initialized file, validate its metadata and cache R and S.
                       Require at least two buffer frames; do not scan all data pages or links.
        record_size  : Return cached R without fetching a page.
        record_count : Read and validate page 1, then return tr; no mutable count is cached.
        insert       : Require R bytes; use the free-list head or allocate and initialize a data page.
                       Insert into its lowest free slot, maintain the free list, increment tr and return RID.
        get          : Check the RID page and expected page format, then return a record byte copy.
        update       : Require R bytes and overwrite an occupied slot in place.
                       Preserve RID, occupancy, page and file counts, and free-page links.
        erase        : Lazy remove: clear occupancy without clearing or relocating the payload.
                       Decrement tr and link a formerly full page once; retain empty data pages.
        scan         : Return a cursor borrowing this object and traversing all data pages.
                       Keep the file alive and unchanged for the lifetime of an active scan.

    helpers:
        meta_data    : Hold decoded metadata values; nfp and tr are local copies, not persistent members.
        encode       : Write the six fields into a writable page without allocation or I/O.
                       Preserve reserved bytes; initialization clears the page before encoding.
        decode(page) : Validate magic, version, R/S, free-head bounds, count bounds and reserved bytes.
                       Return decoded fields; do not verify the entire chain or sum all page counts.
        decode()     : Fetch metadata through bpm, decode it and verify cached R/S still match.
        Insert and erase pin metadata and the target data page before modifying existing records.
        Guard get() marks modified cache pages dirty; writes are flushed by bpm later.
        Releasing a guard only unpins the page; it does not flush or close the file.

    errors:
        Invalid R, insufficient configured frames or repeated initialization -> invalid_argument.
        Unregistered file ID -> invalid_file_handle; malformed metadata, pages or links -> invalid_format.
        RID page outside [2,N) or slot outside [0,S) -> invalid_rid.
        get/update/erase on a valid empty slot -> record_not_found.
        insert/update with a payload length different from R -> invalid_argument.
        Exhausted total-record counter -> resource_limit; unavailable buffer frames -> buffer_exhausted.
        Input, format and required-page fetch checks precede changes to existing records and metadata.
        Allocation, I/O and memory failures propagate; appended pages and partial disk writes are not rolled back.
*/

record_file::meta_data::meta_data(uint32_t _rs,uint32_t _ns,uint64_t _nfp,uint64_t _tr)
{
    rm = record_file_magic;
    v = record_ver;
    rs = _rs;
    ns = _ns;
    nfp = _nfp;
    tr = _tr;
}


void record_file::encode(
        page & p,
        const meta_data& md
    ) noexcept
{
    uint8_t * src = (uint8_t *)p.data();
    /*
    record meta data of the file
    offset      data            descrption
    --------------------------------------
    0        record_file_magic   uint32_t
    4        ver                 uint32_t
    8        record_size         uint32_t
    12       slots               uint32_t
    16       next_free_page      uint64_t
    24       total_records       uint64_t
    */
    write_u32(src,md.rm);
    write_u32(src+ver_offset,md.v);
    write_u32(src+record_size_offset,md.rs);
    write_u32(src+slot_count_offset,md.ns);
    write_u64(src+meta_free_head_offset,md.nfp);
    write_u64(src+meta_total_records_offset,md.tr);
}


record_file::meta_data record_file::decode(const page & p)const
{
    const std::byte * src = p.data();
    uint32_t rm,v,rs,ns;
    uint64_t nfp,tr;
    rm = read_u32(src);
    v = read_u32(src+ver_offset);

    if(rm != record_file_magic || v != record_ver)
        throw db_error(
            error_code::invalid_format,
            "version and magic mismatch in reading file meta data"
        );


    rs = read_u32(src+record_size_offset);
    ns = read_u32(src+slot_count_offset);
    nfp = read_u64(src+meta_free_head_offset);
    tr = read_u64(src+meta_total_records_offset);

    if(rs == 0 || rs > max_record_size || ns != record_page_helper::capacity(rs))
        throw db_error(
            error_code::invalid_format,
            "invalid record layout in file metadata"
        );

    const page_id num_pages = fm_.get_file(fid_).get_num_pages();
    if(num_pages < first_record_data_page ||
        (nfp != invalid_page_id && (nfp < first_record_data_page || nfp >= num_pages)))
        throw db_error(
            error_code::invalid_format,
            "invalid free page head in file metadata"
        );

    // Division avoids overflow when checking the maximum possible record count.
    if(tr / ns + (tr % ns != 0) > num_pages - first_record_data_page)
        throw db_error(
            error_code::invalid_format,
            "record count exceeds data page capacity"
        );

    for(std::size_t i = reserved_offset;i < p.size();++i)
        if(p[i] != std::byte{0})
            throw db_error(
                error_code::invalid_format,
                "nonzero reserved bytes in file metadata"
            );

    return meta_data(rs,ns,nfp,tr);
}

record_file::meta_data record_file::decode() const
{
    const page_guard p = bpm_.fetch_page(fid_,record_meta_page);
    const meta_data md = decode(*p.view());
    if(md.rs != record_size_ || md.ns != slots_per_page_)
        throw db_error(
            error_code::invalid_format,
            "record layout changed after opening the file"
        );
    return md;
}




void record_file::initialize(file_manager& fm,
                             buffer_pool_manager& bpm,
                             file_id fid,
                             std::uint32_t record_size)
{
    const slot_id slots = record_page_helper::capacity(record_size);
    if(bpm.frame_count() < 2)
        throw db_error(
            error_code::invalid_argument,
            "insufficient frames in bufferpool"
        );
    file_handle& fh = fm.get_file(fid);
    if(fh.get_num_pages() != 1)
        throw db_error(
            error_code::invalid_argument,
            "processing un-empty file in init"
        );


    page_guard guard = bpm.new_page(fid);
    page & p = *guard.get();
    p.fill(std::byte{0});
    encode(p,meta_data(record_size,slots));
}




record_file::record_file(
                    file_manager& fm,
                    buffer_pool_manager& bpm,
                    file_id fid
                ):fm_(fm),bpm_(bpm),fid_(fid)
{
    if(bpm.frame_count() < 2)
        throw db_error(
            error_code::invalid_argument,
            "insufficient frames in bufferpool"
        );

    file_handle & fh = fm.get_file(fid);
    page_id num_pages = fh.get_num_pages();
    if(num_pages < 2)
        throw db_error(
            error_code::invalid_format,
            "uninit file can't be used for constructing a record_file"
        );
    page_guard guard = bpm.fetch_page(fid,record_meta_page);
    meta_data md = decode(*guard.view());
    record_size_ = md.rs;
    slots_per_page_ = md.ns;
}


std::uint32_t record_file::record_size() const noexcept
{
    return record_size_;
}

std::uint64_t record_file::record_count() const
{
    return decode().tr;
}



rid record_file::insert(const record_bytes& bytes)
{
    if(bytes.size() != record_size_)
        throw db_error(
            error_code::invalid_argument,
            "unconsistent size between bytes passed in and file record size"
        );

    page_guard meta_guard = bpm_.fetch_page(fid_,record_meta_page,access_type::write);
    meta_data md = decode(*meta_guard.view());
    if(md.rs != record_size_ || md.ns != slots_per_page_)
        throw db_error(
            error_code::invalid_format,
            "record layout changed after opening the file"
        );
    if(md.tr == std::numeric_limits<std::uint64_t>::max())
        throw db_error(
            error_code::resource_limit,
            "record count exhausted"
        );

    rid id;
    if(md.nfp != invalid_page_id)
    {
        page_id pid = md.nfp;
        page_guard data_guard = bpm_.fetch_page(fid_,pid,access_type::write);
        const page & orig_page = *data_guard.view();
        record_page_helper::validate(orig_page,record_size_);

        uint32_t live_count = read_u32(orig_page.data()+live_count_offset);
        uint32_t flags = read_u32(orig_page.data()+flag_offset);
        uint64_t next_free = read_u64(orig_page.data()+next_free_offset);

        const page_id num_pages = fm_.get_file(fid_).get_num_pages();
        if(flags != 1 || live_count >= slots_per_page_ || md.tr < live_count ||
            (next_free != invalid_page_id &&
                (next_free < first_record_data_page || next_free >= num_pages || next_free == pid)))
            throw db_error(
                error_code::invalid_format,
                "invalid free page chain"
            );

        page & p = *data_guard.get();
        std::optional<slot_id> slot = record_page_helper::insert(p,bytes);

        if(slot == std::nullopt)
            throw db_error(
                error_code::invalid_format,
                "fail to insert into file"
            );

        if(live_count + 1 == slots_per_page_)
        {
            md.nfp = next_free;
            write_u32(p.data()+flag_offset,0);
            write_u64(p.data()+next_free_offset,invalid_page_id);
        }
        id.pid = pid;
        id.sid = *slot;
    }
    else
    {
        page_guard data_guard = bpm_.new_page(fid_);
        page_id pid = data_guard.page_no();
        page& p = *data_guard.get();

        record_page_helper::init(p,record_size_);
        std::optional<slot_id> slot = record_page_helper::insert(p,bytes);

        if(!slot)
            throw db_error(
                error_code::internal_error,
                "failed to insert into a new data page"
            );

        if(slots_per_page_ > 1)
        {
            write_u32(p.data()+flag_offset,1);
            write_u64(p.data()+next_free_offset,invalid_page_id);
            md.nfp = pid;
        }
        id.pid = pid;
        id.sid = *slot;
    }
    ++md.tr;
    encode(*meta_guard.get(),md);
    return id;
}




record_bytes record_file::get(rid id) const
{
    page_id pid_ = id.pid;
    slot_id sid_ = id.sid;

    file_handle & fh = fm_.get_file(fid_);
    if(pid_ < first_record_data_page || pid_ >= fh.get_num_pages())
        throw db_error(
            error_code::invalid_rid,
            "record page id out of range"
        );
    page_guard data_guard = bpm_.fetch_page(fid_,pid_);
    record_page_helper::validate(*data_guard.view(),record_size_);

    return record_page_helper::read(*data_guard.view(),sid_);
}



void record_file::update(rid id, const record_bytes& bytes)
{
    if(bytes.size() != record_size_)
        throw db_error(
            error_code::invalid_argument,
            "unconsistent size between bytes passed in and file record size"
        );
    page_id pid_ = id.pid;
    slot_id sid_ = id.sid;

    file_handle & fh = fm_.get_file(fid_);
    if(pid_ < first_record_data_page || pid_ >= fh.get_num_pages())
        throw db_error(
            error_code::invalid_rid,
            "record page id out of range"
        );

    page_guard data_guard = bpm_.fetch_page(fid_,pid_,access_type::write);
    record_page_helper::validate(*data_guard.view(),record_size_);
    record_page_helper::update(*data_guard.get(),sid_,bytes);

}


void record_file::erase(rid id)
{
    page_id pid_ = id.pid;
    slot_id sid_ = id.sid;

    file_handle & fh = fm_.get_file(fid_);
    if(pid_ < first_record_data_page || pid_ >= fh.get_num_pages())
        throw db_error(
            error_code::invalid_rid,
            "record page id out of range"
        );

    page_guard meta_guard = bpm_.fetch_page(fid_,record_meta_page,access_type::write);
    meta_data md = decode(*meta_guard.view());
    if(md.rs != record_size_ || md.ns != slots_per_page_)
        throw db_error(
            error_code::invalid_format,
            "record layout changed after opening the file"
        );

    page_guard data_guard = bpm_.fetch_page(fid_,pid_,access_type::write);
    const page & orig_page = *data_guard.view();
    record_page_helper::validate(orig_page,record_size_);
    if(!record_page_helper::occupied(orig_page,sid_))
        throw db_error(
            error_code::record_not_found,
            "record slot is empty"
        );

    const uint32_t live_count = read_u32(orig_page.data()+live_count_offset);
    const uint32_t flags = read_u32(orig_page.data()+flag_offset);
    const page_id next_free = read_u64(orig_page.data()+next_free_offset);
    const bool was_full = live_count == slots_per_page_;
    const page_id num_pages = fh.get_num_pages();

    if(md.tr < live_count ||
        (was_full && (flags != 0 || next_free != invalid_page_id || md.nfp == pid_)) ||
        (!was_full && (flags != 1 || md.nfp == invalid_page_id ||
            (next_free != invalid_page_id &&
                (next_free < first_record_data_page || next_free >= num_pages || next_free == pid_)))))
        throw db_error(
            error_code::invalid_format,
            "invalid record count or free page chain"
        );

    // Both pages are pinned and all rejection checks precede the lazy removal.
    page & p = *data_guard.get();
    record_page_helper::erase(p,sid_);
    if(was_full)
    {
        write_u32(p.data()+flag_offset,1);
        write_u64(p.data()+next_free_offset,md.nfp);
        md.nfp = pid_;
    }
    --md.tr;
    encode(*meta_guard.get(),md);
}


record_scan record_file::scan() const
{
    return record_scan(*this);
}


/*









*/



record_scan::record_scan(const record_file& file):file_(&file)
{
    file_handle & fh = file.fm_.get_file(file.fid_);
    end_page_ = fh.get_num_pages();

}



record_scan::record_scan(record_scan&& other) noexcept
{
    this->end_page_ = other.end_page_;
    this->file_ = other.file_;
    this->next_page_ = other.next_page_;
    this->next_slot_ = other.next_slot_;
    this->state_ = other.state_;

    other.file_ = nullptr;
    other.next_page_ = first_record_data_page;
    other.next_slot_ = 0;
    other.state_ = scan_state::moved;
}





std::optional<scan_item> record_scan::next()
{
    if(state_ == scan_state::exhausted)
        return std::nullopt;

    if(state_ != scan_state::active)
        throw db_error(
            error_code::invalid_argument,
            "scan is failed or moved"
        );

    try
    {
        while(next_page_ < end_page_)
        {
            page_guard guard = file_->bpm_.fetch_page(file_->fid_,next_page_);
            const page & p = *guard.view();
            record_page_helper::validate(p,file_->record_size_);

            while(next_slot_ < file_->slots_per_page_)
            {
                const slot_id slot = next_slot_;
                uint8_t bytes = (uint8_t)p[header_bytes + slot / 8];
                uint8_t residual = slot % 8;
                if((bytes & (1 << residual)) == 0)
                {
                    ++next_slot_;
                    continue;
                }
                scan_item item;
                item.id.pid = next_page_;
                item.id.sid = next_slot_;
                item.bytes = record_page_helper::read(p,slot);

                ++next_slot_;
                return item;
            }

            ++next_page_;
            next_slot_ = 0;

        }
        state_ = scan_state::exhausted;
        return std::nullopt;
    }
    catch(...)
    {
        state_ = scan_state::failed;
        throw;
    }
}


/*
    CLASS : ROW CODEC
    docs:
        Convert typed rows to fixed-size record bytes and back using an owned column schema.
        columns_ stores types, VARCHAR limits and nullability; offsets_ stores payload positions.
        The constructor computes record_size_ once; encode and decode do not access files or bpm.

    layout:
        [NULL bitmap][column 0 payload][column 1 payload]...
        Each column uses one NULL bit; bit 1 means NULL and bit 0 means a stored value.
        The bitmap occupies ceil(column_count/8) bytes; unused tail bits must be zero.
        INT occupies 4 bytes, FLOAT uses 8-byte IEEE 754 binary64, VARCHAR(n) occupies 2+n bytes.
        Integers and floating-point bit patterns use little-endian encoding.
        VARCHAR starts with a little-endian uint16 length followed by that many raw bytes.
        String lengths are measured in bytes; embedded zero bytes are allowed.
        NULL payloads and unused VARCHAR padding are zero; every row still occupies record_size_ bytes.
        This NULL bitmap is separate from the record page's slot occupancy bitmap.

    methods:
        row_codec   : Validate column definitions and compute bounded payload offsets and row size.
        record_size : Return the encoded size in bytes, excluding record page headers and slot bitmaps.
        encode      : Validate the complete row, allocate zeroed bytes, then encode each value.
                      NULL skips payload encoding; numeric values are not implicitly converted.
        decode      : Validate length, NULL bits, payloads, string bounds, padding and finite floats.
                      Return owned values and strings; preserve empty strings, NULL and signed zero.

    errors:
        Empty schema, invalid type/length declaration or wrong value count -> invalid_argument.
        A row wider than the record page limit -> value_out_of_range.
        Wrong variant alternative -> type_mismatch; disallowed input NULL -> not_null_violation.
        Oversized input strings or non-finite input floats -> value_out_of_range.
        Malformed encoded bytes, including illegal stored NULL -> invalid_format.
        Unsupported byte or double representation -> feature_unsupported during construction.
        Inputs are never modified; allocation failures propagate without publishing partial results.
*/



row_codec::row_codec(std::vector<column_spec> columns):columns_(columns)
{
    if(CHAR_BIT != 8 || sizeof(double) != 8 ||
        !std::numeric_limits<double>::is_iec559 ||
        std::numeric_limits<double>::digits != 53 ||
        std::numeric_limits<double>::max_exponent != 1024)
        throw db_error(
            error_code::feature_unsupported,
            "row encoding requires 8-bit bytes and IEEE 754 binary64"
        );

    if(columns.size() == 0)
        throw db_error(
            error_code::invalid_argument,
            "column list must not be empty"
        );

    std::size_t bitmap_bytes = (columns.size()+7) / 8;
    if(bitmap_bytes > max_record_size)
        throw db_error(
            error_code::value_out_of_range,
            "null bitmap exceeds record size limit"
        );

    offsets_.reserve(columns.size());
    record_size_ = static_cast<std::uint32_t>(bitmap_bytes);
    for(std::size_t i = 0;i < columns.size();++i)
    {
        data_type type = columns[i].type;
        uint32_t to_increase = 0;

        if(type == data_type::INT || type == data_type::FLOAT)
        {
            if(columns[i].max_length != 0)
                throw db_error(
                    error_code::invalid_argument,
                    "numeric columns max_lengths must be zero"
                );

            to_increase = (type == data_type::INT)?4:8;
        }
        else if(type == data_type::VARCHAR)
        {
            if(columns[i].max_length == 0 || columns[i].max_length > 65535)
                throw db_error(
                    error_code::invalid_argument,
                    "varchar max_length must be within 1..65535"
                );
            to_increase = columns[i].max_length + 2;
        }
        else
            throw db_error(
                error_code::invalid_argument,
                "invalid column type"
            );

        if(to_increase > max_record_size - record_size_)
            throw db_error(
                error_code::value_out_of_range,
                "record does not fit in a page"
            );

        offsets_.push_back(record_size_);
        record_size_ += to_increase;
    }

}


std::uint32_t row_codec::record_size() const noexcept
{
    return record_size_;
}



record_bytes row_codec::encode(const row& values) const
{
    if(values.size() != columns_.size())
        throw db_error(
            error_code::invalid_argument,
            "value count does not match column count"
        );


    for(std::size_t i = 0;i < values.size();++i)
    {
        const value& val = values[i];
        const column_spec & col = columns_[i];
        if(std::holds_alternative<std::monostate>(val))
        {
            if(!col.nullable)
                throw db_error(
                    error_code::not_null_violation,
                    "require not null value"
                );
            continue;
        }
        bool correct_type = (col.type == data_type::INT && std::holds_alternative<std::int32_t>(val))   ||
                            (col.type == data_type::FLOAT && std::holds_alternative<double>(val))       ||
                            (col.type == data_type::VARCHAR && std::holds_alternative<std::string>(val));
        if(!correct_type)
            throw db_error(
                error_code::type_mismatch,
                "value type does not match column type"
            );
        if(col.type == data_type::FLOAT && !std::isfinite(std::get<double>(val)))
            throw db_error(
                error_code::value_out_of_range,
                "float value must be finite"
            );
        if(col.type == data_type::VARCHAR &&
            std::get<std::string>(val).size() > col.max_length)
            throw db_error(
                error_code::value_out_of_range,
                "string exceeds column maximum length"
            );
    }

    record_bytes ret(this->record_size_,std::byte{0});

    for(std::size_t i = 0;i < values.size();++i)
    {
        const value& val = values[i];
        const column_spec & col = columns_[i];
        if(std::holds_alternative<std::monostate>(val))
        {
            std::size_t bytes = i / 8;
            std::size_t resid = i % 8;
            ret[bytes] |= ((std::byte)1 << resid);
            continue;
        }

        std::byte * dest = ret.data() + offsets_[i];
        data_type type = col.type;

        switch(type)
        {
        case data_type::INT:
        {
            std::int32_t num = std::get<std::int32_t>(val);
            write_u32(dest,static_cast<std::uint32_t>(num));
        };break;
        case data_type::FLOAT:
        {
            double num = std::get<double>(val);
            uint64_t bits = 0;
            std::memcpy(&bits,&num,sizeof(bits));
            write_u64(dest,bits);
        };break;
        case data_type::VARCHAR:
        {
            const std::string& text = std::get<std::string>(val);
            std::uint16_t len = static_cast<std::uint16_t>(text.size());
            dest[0] = (std::byte)(len & 0xffu);
            dest[1] = (std::byte)((len >> 8) & 0xffu);
            if(!text.empty())
                std::memcpy(dest + 2,text.data(),text.size());
        };break;
        default: throw db_error(
            error_code::internal_error,
            "unexpected type occurred while encoding"
        );
        }


    }

    return ret;


}





row row_codec::decode(const record_bytes& bytes) const
{
    if(bytes.size() != record_size_)
        throw db_error(
            error_code::invalid_format,
            "mismatch of expected format of row and what passed in"
        );
    const std::size_t bitmap_bytes = (columns_.size()+7) / 8;
    const std::size_t used_bits = columns_.size() % 8;
    if(used_bits != 0 &&
        (std::to_integer<unsigned>(bytes[bitmap_bytes-1]) >> used_bits) != 0)
        throw db_error(
            error_code::invalid_format,
            "nonzero unused null bitmap bits"
        );

    row ret(columns_.size());


    for(std::size_t i = 0;i < columns_.size();++i)
    {
        const db::column_spec & col = columns_[i];
        data_type type = col.type;
        std::uint64_t byte = i / 8;
        std::uint64_t resid = i % 8;
        std::byte mask = (bytes[byte] & (std::byte)(1 << resid));
        if(mask != (std::byte)(0))
        {
            if(!col.nullable)
                throw db_error(
                    error_code::invalid_format,
                    "null found in a non-nullable column"
                );
            const std::size_t end = i + 1 < columns_.size() ? offsets_[i+1] : record_size_;
            for(std::size_t j = offsets_[i];j < end;++j)
                if(bytes[j] != std::byte{0})
                    throw db_error(
                        error_code::invalid_format,
                        "nonzero payload in a null column"
                    );
            ret[i] = std::monostate{};
            continue;
        }

        switch(type)
        {
        case data_type::INT:
        {
            uint32_t bits = read_u32(bytes.data() + offsets_[i]);
            std::int64_t num = bits <= 0x7fffffffu
                ? static_cast<std::int64_t>(bits)
                : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
            ret[i] = static_cast<std::int32_t>(num);
        };break;
        case data_type::FLOAT:
        {
            uint64_t bits = read_u64(bytes.data() + offsets_[i]);
            double num = 0;
            std::memcpy(&num,&bits,sizeof(num));
            if(!std::isfinite(num))
                throw db_error(
                    error_code::invalid_format,
                    "non-finite float in stored record"
                );
            ret[i] = num;
        };break;
        case data_type::VARCHAR:
        {
            uint16_t lower,upper;
            lower = uint16_t(bytes[offsets_[i]]);
            upper = uint16_t(bytes[offsets_[i] + 1]) << 8;
            uint16_t len = lower | upper;
            if(len > col.max_length)
                throw db_error(
                    error_code::invalid_format,
                    "stored string exceeds column maximum length"
                );
            for(std::size_t j = len;j < col.max_length;++j)
                if(bytes[offsets_[i]+2+j] != std::byte{0})
                    throw db_error(
                        error_code::invalid_format,
                        "nonzero varchar padding in stored record"
                    );
            std::string text;
            text.resize(len);
            if(len != 0)
                std::memcpy(text.data(),bytes.data()+offsets_[i]+2,len);
            ret[i] = std::move(text);

        };break;
        default: throw db_error(
            error_code::internal_error,
            "unexpected type occured while decoding"
        );
        }

    }


    return ret;
}











































































}
