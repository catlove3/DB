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
    /**
     * @brief 构造拥有错误码、消息和上下文的异常对象，不执行日志输出或恢复。
     * @param code 调用者按稳定枚举分类；禁止按 message 的文字判断错误。
     * @param message 诊断文本，由 std::runtime_error 保存，措辞不作为兼容承诺。
     * @param context 按值接收并保存；允许为空，未提供的可选字段保持缺省。
     * @note 字符串/异常存储分配可能抛出 std::bad_alloc；构造函数不保证 noexcept。
     */
    db_error(error_code code, std::string message, error_context context = {});
    /** @return 构造时保存的错误码；不根据消息、errno 或上下文重新推断。 */
    error_code code() const noexcept;
    /**
     * @return 借用的只读上下文引用，不能超过此异常对象的生命周期。
     * @note 各字段是否存在取决于抛出点；operation 也可能为空，不能假定路径、
     *       页号和系统错误码总是齐全。访问本身不分配、不修改异常。
     */
    const error_context& context() const noexcept;

private:
    error_code code_;
    error_context context_;
};

/**
 * @brief 返回用于诊断的枚举名称。
 * @param code 已知枚举值返回对应标识符，未识别值返回 "unknown_error"。
 * @return 静态字符串的借用指针，无需释放；函数不分配、不抛异常。
 */
const char* error_code_name(error_code code) noexcept;

} // namespace db
