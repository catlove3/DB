#include <db/catalog/catalog.h>
#include <db/common/error.h>
#include <db/storage/file_manager.h>
#include <db/storage/buffer_pool.h>

namespace db
{

// Runtime state only; none of these handles or borrow counts belong in catalog JSON.
struct database_manager::opened_table
{
    table_schema schema;
    file_id fid = 0;
    std::unique_ptr<record_file> records;
    std::unique_ptr<row_codec> codec;
    std::size_t lease_count = 0;
};

/*
    IMPLEMENTATION ORDER
    1. Choose and vendor a JSON library; keep its types private to this file.
    2. Encode/decode root_catalog and database_catalog; validate IDs, names, paths and typed defaults.
    3. Implement catalog_store initialization, loads and publication through temporary files.
    4. Implement create/list/use_database, then create/list/describe_table.
    5. Implement open_table registrations and table_lease move/release/accessors.
    6. Implement explicit close, drop_table and drop_database with publication progress.

    Until indexes and constraints are supported, reject their definitions with feature_unsupported.
    Bodies are intentionally left for the next lessons; do not add successful no-op implementations.
*/

// ---------- Catalog serialization and filesystem helpers ----------


// ---------- catalog_store ----------


// ---------- database_manager ----------


// ---------- table_lease ----------


}
