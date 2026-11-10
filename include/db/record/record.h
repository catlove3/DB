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
    static void assess(const page & p,slot_id id,bool should_exist);
    static void get_offset_and_resid(slot_id slot,slot_id & byte_offset,slot_id& bit_offset) noexcept;
    static std::size_t get_record_offset(const page & p,slot_id slot) noexcept;
    static std::optional<slot_id> find_free(const page & p) noexcept;
public:
    static void clear(page&p);
    static slot_id capacity(slot_id record_size);
    static void init(page& p, slot_id record_size);
    static void validate(const page& p,std::uint32_t expected_record_size);
    static slot_id record_count(const page& p);
    static bool occupied(const page& p, slot_id slot);
    static std::optional<slot_id> first_free(const page& p);
    static record_bytes read(const page& p, slot_id slot);
    static std::optional<slot_id> insert(page& p,const record_bytes& bytes);
    static void update(page& p,slot_id slot,const record_bytes& bytes);
    static void erase(page& p, slot_id slot);
};

class record_scan;

class record_file {
private:
    friend class record_scan;

    file_manager& fm_;
    buffer_pool_manager& bpm_;
    file_id fid_;

    std::uint32_t record_size_;
    std::uint32_t slots_per_page_;

    #pragma pack(push,1)
    struct meta_data{
        uint32_t rm;
        uint32_t v;
        uint32_t rs;
        uint32_t ns;
        uint64_t nfp;
        uint64_t tr;
        meta_data(uint32_t _rs,uint32_t _ns,uint64_t _nfp = invalid_page_id,uint64_t _tr = 0);
    };
    #pragma pack(pop)

    static void encode(
        page & p,
        const meta_data& md
    ) noexcept;

    meta_data decode(
        const page & p
    )const;

    meta_data decode()const;

public:
    static void initialize(
        file_manager& fm,
        buffer_pool_manager& bpm,
        file_id fid,
        std::uint32_t record_size
    );

    record_file(
        file_manager& fm,
        buffer_pool_manager& bpm,
        file_id fid
    );

    std::uint32_t record_size() const noexcept;
    std::uint64_t record_count() const;

    rid insert(const record_bytes& bytes);

    record_bytes get(rid id) const;
    void update(rid id, const record_bytes& bytes);
    void erase(rid id);

    record_scan scan() const;

    record_file(const record_file&) = delete;
    record_file& operator=(const record_file&) = delete;
    record_file(record_file&&) = delete;
    record_file& operator=(record_file&&) = delete;
};

class record_scan {
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

    page_id end_page_ = first_record_data_page;

    scan_state state_ = scan_state::active;

public:
    explicit record_scan(const record_file& file);

    std::optional<scan_item> next();

    record_scan(const record_scan&) = delete;
    record_scan& operator=(const record_scan&) = delete;

    record_scan(record_scan&& other) noexcept;
    record_scan& operator=(record_scan&&) = delete;
};

struct column_spec {
    data_type type;

    // Maximum VARCHAR bytes; numeric columns require 0.
    std::uint32_t max_length = 0;

    bool nullable = true;
};

class row_codec {
private:
    std::vector<column_spec> columns_;
    std::vector<std::uint32_t> offsets_;
    std::uint32_t record_size_;
public:
    explicit row_codec(std::vector<column_spec> columns);

    std::uint32_t record_size() const noexcept;

    record_bytes encode(const row& values) const;

    row decode(const record_bytes& bytes) const;
};
};
