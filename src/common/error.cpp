#include "db/common/error.h"

#include <utility>

namespace db {

db_error::db_error(error_code code, std::string message, error_context context)
    : std::runtime_error(std::move(message)), code_(code), context_(std::move(context)) {}

error_code db_error::code() const noexcept { return code_; }
const error_context& db_error::context() const noexcept { return context_; }

const char* error_code_name(error_code code) noexcept {
    switch (code) {
#define IDBS_ERROR_NAME(name) case error_code::name: return #name
        IDBS_ERROR_NAME(invalid_argument);
        IDBS_ERROR_NAME(syntax_error);
        IDBS_ERROR_NAME(feature_unsupported);
        IDBS_ERROR_NAME(no_database);
        IDBS_ERROR_NAME(object_not_found);
        IDBS_ERROR_NAME(object_exists);
        IDBS_ERROR_NAME(ambiguous_column);
        IDBS_ERROR_NAME(type_mismatch);
        IDBS_ERROR_NAME(value_out_of_range);
        IDBS_ERROR_NAME(not_null_violation);
        IDBS_ERROR_NAME(invalid_rid);
        IDBS_ERROR_NAME(record_not_found);
        IDBS_ERROR_NAME(duplicate_key);
        IDBS_ERROR_NAME(foreign_key_violation);
        IDBS_ERROR_NAME(primary_key_definition);
        IDBS_ERROR_NAME(dependency_exists);
        IDBS_ERROR_NAME(invalid_file_handle);
        IDBS_ERROR_NAME(invalid_page);
        IDBS_ERROR_NAME(invalid_format);
        IDBS_ERROR_NAME(io_error);
        IDBS_ERROR_NAME(buffer_exhausted);
        IDBS_ERROR_NAME(resource_busy);
        IDBS_ERROR_NAME(resource_limit);
        IDBS_ERROR_NAME(internal_error);
#undef IDBS_ERROR_NAME
    }
    return "unknown_error";
}

} // namespace db
