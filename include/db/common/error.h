#pragma once

#include "db/common/types.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace db {

// Compare codes, never exception message text. SQL-related codes are reserved
// for the later layers; storage uses the same vocabulary from the start.
enum class error_code {
    invalid_argument, syntax_error, feature_unsupported,
    no_database, object_not_found, object_exists, ambiguous_column,
    type_mismatch, value_out_of_range, not_null_violation,
    invalid_rid, record_not_found,
    duplicate_key, foreign_key_violation, primary_key_definition,
    dependency_exists,
    invalid_file_handle, invalid_page, invalid_format, io_error,
    buffer_exhausted, resource_busy, resource_limit, internal_error
};

struct error_context {
    std::string operation;
    std::optional<std::string> path;
    std::optional<std::uint64_t> object_id;
    std::optional<page_id> page;
    std::optional<std::uint32_t> slot;
    std::optional<std::size_t> line;
    std::optional<std::size_t> column;
    std::optional<int> os_error;
    // Preserve the original failure if cleanup also fails.
    std::optional<int> cleanup_os_error;
};

class db_error : public std::runtime_error {
public:
    db_error(error_code code, std::string message, error_context context = {});
    error_code code() const noexcept;
    const error_context& context() const noexcept;

private:
    error_code code_;
    error_context context_;
};

const char* error_code_name(error_code code) noexcept;

} // namespace db
