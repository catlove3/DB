#pragma once

#include <db/common/types.h>
#include <db/record/record.h>
#include <filesystem>
#include <unordered_map>
#include <memory>
#include <optional>

namespace db
{

class file_manager;
class buffer_pool_manager;
class database_manager;
class ddl_service;

constexpr std::uint32_t catalog_format_version = 1;
constexpr int json_indent = 4;
inline const std::filesystem::path data_dir = std::filesystem::path(__FILE__)
                                                .parent_path()
                                                .parent_path()
                                                .parent_path()
                                                .parent_path() / "data";
inline const std::filesystem::path db_dir = data_dir / "databases";

// ---------- Persistent schema snapshots ----------

struct column_schema
{
    column_id id = invalid_column_id;
    std::string name;
    std::uint32_t ordinal = 0;
    data_type type = data_type::INT;
    std::uint32_t max_length = 0;
    // Preserve the declaration; a primary key can make effective nullability false.
    bool declared_nullable = true;
    bool has_default = false;
    value default_value = std::monostate{};
};

enum class index_origin
{
    explicit_index,
    primary_key,
    foreign_key,
    unique
};

enum class index_null_policy
{
    skip_nulls
};

enum class index_state
{
    building,
    ready
};

// Index and constraint definitions belong to their containing table_schema.
// Their structures are reserved now; operational support is added in later stages.
struct index_schema
{
    index_id id = invalid_index_id;
    std::string name;
    std::vector<column_id> columns;
    index_origin origin = index_origin::explicit_index;
    index_null_policy null_policy = index_null_policy::skip_nulls;
    index_state state = index_state::building;
    std::filesystem::path file_path;
};

struct primary_key_schema
{
    constraint_id id = invalid_constraint_id;
    std::string name;
    std::vector<column_id> columns;
    index_id index = invalid_index_id;
};

struct foreign_key_schema
{
    constraint_id id = invalid_constraint_id;
    std::string name;
    std::vector<column_id> child_columns;
    table_id parent_table = invalid_table_id;
    std::vector<column_id> parent_columns;
    index_id child_index = invalid_index_id;
};

struct table_schema
{
    table_id id = invalid_table_id;
    std::string name;
    std::vector<column_schema> columns;
    std::uint32_t record_format_version = 1;
    // Paths are relative to databases/<database_id>/, never derived from SQL names.
    std::filesystem::path heap_path;
    std::vector<index_schema> indexes;
    std::optional<primary_key_schema> primary_key;
    std::vector<foreign_key_schema> foreign_keys;
};

struct database_info
{
    database_id id = invalid_database_id;
    std::string name;
};

struct table_info
{
    table_id id = invalid_table_id;
    std::string name;
};

struct root_catalog
{
    std::uint32_t format_version = catalog_format_version;
    database_id next_database_id = 1;
    std::vector<database_info> databases;
};

struct database_catalog
{
    std::uint32_t format_version = catalog_format_version;
    database_id id = invalid_database_id;
    // Tables, indexes and constraints share this database-local ID allocator.
    std::uint64_t next_object_id = 1;
    std::vector<table_schema> tables;
};

// ---------- Requests without allocated persistent IDs ----------

struct column_definition
{
    std::string name;
    data_type type = data_type::INT;
    std::uint32_t max_length = 0;
    bool nullable = true;
    bool has_default = false;
    value default_value = std::monostate{};
};

struct primary_key_definition
{
    std::vector<std::string> columns;
    std::optional<std::string> name;
};

struct foreign_key_definition
{
    std::vector<std::string> child_columns;
    std::string parent_table;
    std::vector<std::string> parent_columns;
    std::optional<std::string> name;
};

struct create_table_request
{
    std::string name;
    std::vector<column_definition> columns;
    // Until constraint support is implemented, reject these with feature_unsupported.
    std::optional<primary_key_definition> primary_key;
    std::vector<foreign_key_definition> foreign_keys;
};

class catalog_store
{
private:
    std::filesystem::path data_lord;
public:
    explicit catalog_store(std::filesystem::path data_root = data_dir);

    void init_meta();
    root_catalog load_root() const;
    database_catalog load_database(database_id id) const;
    void save_root(const root_catalog& snapshot);
    void save_database(const database_catalog& snapshot);
    const std::filesystem::path& data_root() const noexcept;
};

class table_lease
{
private:
    friend class database_manager;
    // Adopt a borrow already counted by database_manager::open_table.
    table_lease(database_manager& manager,table_id id) noexcept;

    database_manager* manager_ = nullptr;
    table_id table_ = invalid_table_id;
public:
    ~table_lease() noexcept;
    table_lease(const table_lease&) = delete;
    table_lease& operator=(const table_lease&) = delete;
    table_lease(table_lease&& other) noexcept;
    table_lease& operator=(table_lease&& other) noexcept;

    record_file& records() const;
    const row_codec& codec() const;
    const table_schema& schema() const;
    bool valid() const noexcept;
    void release() noexcept;

};

class database_manager
{
private:
    friend class table_lease;
    friend class ddl_service;
    struct opened_table;

    database_catalog& require_database();
    const database_catalog& require_database() const;
    opened_table& require_open_table(table_id id);
    const opened_table& require_open_table(table_id id) const;
    void release_table(table_id id) noexcept;
    void close_table(table_id id);

    catalog_store& store_;
    file_manager& fm_;
    buffer_pool_manager& bpm_;
    root_catalog root_;
    std::optional<database_catalog> current_;
    std::unordered_map<table_id,std::unique_ptr<opened_table>> open_tables_;
public:
    database_manager(catalog_store& store,file_manager& fm,buffer_pool_manager& bpm);
    // Explicit close reports errors; destruction performs best-effort cleanup only.
    ~database_manager() noexcept;
    database_manager(const database_manager&) = delete;
    database_manager& operator=(const database_manager&) = delete;
    database_manager(database_manager&&) = delete;
    database_manager& operator=(database_manager&&) = delete;

    database_id create_database(const std::string& name,statement_context& ctx);
    void drop_database(database_id id,statement_context& ctx);
    void use_database(database_id id);
    std::vector<database_info> list_databases() const;
    std::optional<database_id> current_database() const noexcept;

    table_id create_table(const create_table_request& request,statement_context& ctx);
    void drop_table(table_id id,statement_context& ctx);
    table_schema describe_table(table_id id) const;
    std::vector<table_info> list_tables() const;
    table_lease open_table(table_id id);
    void close();

};

}
