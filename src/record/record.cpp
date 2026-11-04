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
        管理一张内存页中的定长记录，只解释和修改 page 字节。
        不负责文件 I/O、缓存页生命周期、字段解码或跨页空闲链表。

    layout:
        [64-byte header][occupied bitmap][fixed-size record slots]
        R = record_size，单位为 byte，合法范围为 1..8127。
        S = num_slots，取满足 64 + ceil(S/8) + S*R <= page_size 的最大值。
        B = (S + 7)/8，槽 slot 的记录偏移为 64 + B + slot*R。
        槽号从 0 开始，位图最低位对应本字节中的第一个槽，1 表示占用。
        页头整数使用小端编码，magic 为 RMP1，version 为 1。
        live_count 必须等于有效位图中 1 的数量；保留字节和无效尾位为 0。
        flags 和 next_free 由文件层维护，页内 CRUD 不修改它们。

    methods:
        clear        : 只清零整页；清零后还不是合法记录页。
        capacity     : 根据记录字节数计算槽容量，非法长度抛 invalid_argument。
        init         : 先验证长度，再清页并写入合法空页头。
        validate     : 校验页格式、预期记录长度、容量、位图及计数，不修改页。
        record_count : 校验后返回当前有效记录数。
        occupied     : 检查指定槽的占用位，合法空槽返回 false。
        first_free   : 返回最小空槽号；页满返回 nullopt；槽号 0 是有效结果。
        read         : 返回目标记录的独立字节副本，不借用页内存。
        insert       : 写入最小空槽、置占用位并增加计数；页满返回 nullopt。
        update       : 覆盖原槽载荷，保持槽号、占用位和计数不变。
        erase        : lazy remove，只清占用位并减少计数。
                       保留旧载荷，禁止通过 read 读取，复用槽时由 insert 覆盖。

    helpers:
        check_page           : 检查页内记录长度，再调用 validate。
        assess               : 检查槽号范围和期望占用状态。
        get_offset_and_resid : 计算占用位所在的页内字节偏移和 bit 偏移。
        get_record_offset    : 计算记录载荷的页内字节偏移。
        find_free            : 在已验证的页内找空槽，避免重复完整校验。
        除 check_page 外，以上辅助函数均要求页已经通过格式检查；
        get_record_offset 还要求槽号合法。

    errors:
        页格式损坏 -> invalid_format；槽号越界 -> invalid_rid。
        合法空槽上的 read/update/erase -> record_not_found。
        insert/update 输入长度不等于 R -> invalid_argument。
        所有拒绝检查在修改页前完成；被拒绝时原页字节不变。
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
