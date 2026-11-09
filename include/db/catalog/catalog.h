#pragma once

#include <db/common/types.h>
#include <db/record/record.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>

namespace db
{

class file_manager;
class buffer_pool_manager;
class database_manager;
class ddl_service;

constexpr std::uint32_t catalog_format_version = 1;

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

/*
    CLASS : CATALOG STORE
    docs:
        Own the data-root path and persist catalog value snapshots as JSON.
        JSON types and serialization helpers stay private to catalog.cpp.
        All methods below are declarations; persistence is not implemented yet.

    layout:
        <root>/engine.meta.json                      -> root_catalog
        <root>/databases/<database_id>/catalog.json  -> database_catalog
        <root>/databases/<database_id>/tables/<table_id>.heap
        <root>/databases/<database_id>/indexes/<index_id>.bpt
        Schemas contain persistent IDs and relative paths, never runtime file_id values.

    methods:
        initialize    : Create a fresh empty catalog; reject an existing root catalog.
        load_root     : Read and validate an independent root snapshot.
        load_database : Require a registered ID; validate schema, paths and referenced files.
        save_root     : Prepare JSON, write and close a temporary file, then replace the root file.
        save_database : Publish a complete database snapshot using the same replacement protocol.
        data_root     : Return the owned root path by reference; do not perform I/O.

    errors:
        Missing initial catalog -> object_not_found; malformed registered data -> invalid_format.
        Actual filesystem errors -> io_error; allocation failures propagate.
        The caller serializes catalog writes. Successful replacement must return without later throws.
*/
class catalog_store
{
public:
    explicit catalog_store(std::filesystem::path data_root);

    void initialize();
    root_catalog load_root() const;
    database_catalog load_database(database_id id) const;
    void save_root(const root_catalog& snapshot);
    void save_database(const database_catalog& snapshot);
    const std::filesystem::path& data_root() const noexcept;

private:
    std::filesystem::path data_root_;
};

/*
    CLASS : TABLE LEASE
    docs:
        Borrow one registered open table from database_manager.
        The manager owns schema, codec and records; it must outlive all leases.
        Moving transfers one borrow; release and destruction only reduce the borrow count.
        Active leases block DROP, database switching and explicit closure of the table.
        These lifetime rules are the implementation contract; methods are not implemented yet.

    methods:
        records : Access the record_file used for CRUD and scans.
        codec   : Access the row codec built from the effective column definitions.
        schema  : Access the complete table definition while the lease remains valid.
        valid   : Report whether this lease still owns a borrow.
        release : Give up the borrow without closing files; repeated release is harmless.

    errors:
        Access through a released or moved lease -> invalid_argument.
        Accessor references cannot outlive the lease; the manager closes files explicitly.
*/
class table_lease
{
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

private:
    friend class database_manager;
    // Adopt a borrow already counted by database_manager::open_table.
    table_lease(database_manager& manager,table_id id) noexcept;

    database_manager* manager_ = nullptr;
    table_id table_ = invalid_table_id;
};

/*
    CLASS : DATABASE MANAGER
    docs:
        Coordinate catalog publication, the current database and open heap registrations.
        Borrow the store and both storage managers; all three must outlive this object.
        Reuse one open-table registration per table and keep its address stable for leases.
        This is an interface scaffold; database and table operations are not implemented yet.

    methods:
        create_database : Prepare a new ID-based directory and publish the root; do not select it.
        use_database    : Prepare the target, reject active leases, close old tables and switch.
        list_databases  : Return ID/name snapshots sorted by name, then ID; no selection required.
        create_table    : Validate columns/defaults, initialize and close the heap, then publish schema.
        describe_table  : Return a complete table-schema copy for the current database.
        list_tables     : Return current-database ID/name snapshots in stable order.
        open_table      : Reuse or prepare records and codec, check equal row sizes, return a lease.
        drop_table      : Check references/leases, close resources, publish removal, then reclaim files.
        drop_database   : Apply the same ordering; clear selection when dropping the current database.
        current_database: Return the selected persistent ID, or nullopt when no database is selected.
        close           : Close registered heaps through bpm; reject outstanding leases.

    errors:
        Missing selection -> no_database; duplicate/missing names or IDs -> object_exists/object_not_found.
        Active leases -> resource_busy; unsupported index/constraint definitions -> feature_unsupported.
        Mutating methods receive caller-owned statement_context and mark metadata_published after publication.
        Publication and cleanup are separate phases; a cleanup error cannot undo a published catalog.
*/
class database_manager
{
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
    std::map<table_id,std::unique_ptr<opened_table>> open_tables_;
};

}
