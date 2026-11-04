#pragma once

#include<db/common/types.h>
#include<optional>

namespace db
{
class file_manager;
class buffer_pool_manager;



class record_page_helper
{
private:
    static void check_page(const page & p);
    // 以下辅助函数只用于已经通过格式检查的页。
    static void assess(const page & p,slot_id id,bool should_exist);
    static void get_offset_and_resid(slot_id slot,slot_id & byte_offset,slot_id& bit_offset) noexcept;
    static std::size_t get_record_offset(const page & p,slot_id slot) noexcept;
    static std::optional<slot_id> find_free(const page & p) noexcept;
public:
    // 仅清零字节；清零后需要 init 才是合法记录页。
    static void clear(page&p);
    // 给定记录长度，计算一页最多能放多少条
    static slot_id capacity(slot_id record_size);
    // 将一张页初始化为空记录页
    static void init(page& p, slot_id record_size);
    // 校验页格式，并检查记录长度是否符合预期
    static void validate(const page& p,std::uint32_t expected_record_size);
    // 当前实际存放的记录数量。
    static slot_id record_count(const page& p);
    // 指定槽是否有记录；槽号越界应报错。
    static bool occupied(const page& p, slot_id slot);
    // 找到最小的空槽号；页满返回 nullopt。
    static std::optional<slot_id> first_free(const page& p);
    // 读取指定槽，返回独立的记录副本。
    static record_bytes read(const page& p, slot_id slot);
    // 插入记录，返回槽号；页满返回 nullopt。
    static std::optional<slot_id> insert(page& p,const record_bytes& bytes);
    // 盖指定槽的记录，保持槽号和记录数量不变。
    static void update(page& p,slot_id slot,const record_bytes& bytes);
    // 删除时只清占用位并减少计数，载荷留到槽复用时覆盖。
    static void erase(page& p, slot_id slot);
};




class record_scan;

class record_file {
public:
    // 给只有 page 0 的新文件创建记录元数据页。
    static void initialize(
        file_manager& fm,
        buffer_pool_manager& bpm,
        file_id fid,
        std::uint32_t record_size
    );

    // 绑定已经初始化的记录文件，读取并验证元数据。
    record_file(
        file_manager& fm,
        buffer_pool_manager& bpm,
        file_id fid
    );

    std::uint32_t record_size() const noexcept;
    std::uint64_t record_count() const;

    // 文件层可分配新页，因此成功时直接返回 RID。
    rid insert(const record_bytes& bytes);

    record_bytes get(rid id) const;
    void update(rid id, const record_bytes& bytes);
    void erase(rid id);

    record_scan scan() const;

    // 扫描器会借用本对象，保持对象身份和地址稳定。
    record_file(const record_file&) = delete;
    record_file& operator=(const record_file&) = delete;
    record_file(record_file&&) = delete;
    record_file& operator=(record_file&&) = delete;

private:
    friend class record_scan;

    // 借用，不负责销毁管理器或关闭文件。
    file_manager& fm_;
    buffer_pool_manager& bpm_;
    file_id fid_;

    // 构造时验证并保存的固定布局信息。
    std::uint32_t record_size_;
    std::uint32_t slots_per_page_;

    // free_head 和 total_records 每次从元数据页读取，
    // 不在对象中额外保存可变副本。
};

// ---------- 记录扫描 ----------

class record_scan {
public:
    explicit record_scan(const record_file& file);

    // 每次返回一条独立记录；扫描结束返回 nullopt。
    std::optional<scan_item> next();

    record_scan(const record_scan&) = delete;
    record_scan& operator=(const record_scan&) = delete;

    record_scan(record_scan&& other) noexcept;
    record_scan& operator=(record_scan&&) = delete;

private:
    enum class scan_state {
        active,
        exhausted,
        failed,
        moved
    };

    const record_file* file_ = nullptr;

    page_id next_page_ = first_record_data_page;
    slot_id next_slot_ = 0;

    // 构造时捕获的文件页数，扫描范围不包含此页号。
    page_id end_page_ = first_record_data_page;

    scan_state state_ = scan_state::active;

    // 不长期持有 page_guard。
};

// ---------- 列定义与行编码 ----------

struct column_spec {
    data_type type;

    // VARCHAR 的最大字节数；INT/FLOAT 必须为 0。
    std::uint32_t max_length = 0;

    bool nullable = true;
};

class row_codec {
public:
    // 验证列定义，计算每列偏移和整条记录长度。
    explicit row_codec(std::vector<column_spec> columns);

    std::uint32_t record_size() const noexcept;

    // 字段值 → 定长记录字节。
    record_bytes encode(const row& values) const;

    // 定长记录字节 → 字段值。
    row decode(const record_bytes& bytes) const;

private:
    std::vector<column_spec> columns_;
    std::vector<std::uint32_t> offsets_;
    std::uint32_t record_size_;
};





};
