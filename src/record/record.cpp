#include <db/record/record.h>
#include <db/storage/file_manager.h>
#include <cstring>

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
    constexpr std::uint32_t record_ver = 1;
    constexpr std::size_t max_record_size = page_size - header_bytes - 1;
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

}
