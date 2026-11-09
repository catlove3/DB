#include <db/catalog/catalog.h>

/*
    CATALOG TEST PLAN
    Add real tests as each implementation stage becomes available.
    This file has no main and is not registered with CTest yet.

    1. Root/database catalog serialization round trips and explicit INT/FLOAT defaults.
    2. Invalid versions, duplicate IDs/names, missing registered files and escaping paths.
    3. Initial creation, atomic replacement failures and normal restart persistence.
    4. Create/list/use two databases; create/list/describe tables and reject duplicate columns.
    5. Repeated open_table shares a registration; moved/released leases preserve borrow counts.
    6. Active leases block DROP/USE/close; released tables can close and reopen normally.
    7. Unsupported constraints fail explicitly before a new table is published.
    8. DROP publishes before reclaiming owned files; cleanup errors preserve publication progress.

    Once runnable, add unit_catalog linked against db_catalog in CMakeLists.txt.
*/
