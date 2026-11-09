#include <db/record/record.h>
#include <db/storage/file_manager.h>
#include <db/storage/buffer_pool.h>
#include <cstring>
#include <limits>

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
    Bind an open file to its fixed-size records without owning its lifetime.
    Page 1 stores RMF1 metadata; record data pages start at page 2.
    Read the free-page head and total count from metadata on each operation.
    Insert pins metadata before changing data and removes a full head page from the free list.
    Get and update preserve record positions and counts.
    Erase uses lazy remove, decrements the total count and links a formerly full page once.
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














}
