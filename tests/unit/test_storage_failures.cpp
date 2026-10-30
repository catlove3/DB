#include "db/common/error.h"
#include "db/storage/file_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

// Link this Linux test with --wrap=close,pread,pwrite,unlink. The production
// library still calls real syscalls; only this executable injects failures.
extern "C" int __real_close(int);
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" int __real_unlink(const char*);

namespace {

struct fault_state {
    bool close_error = false;
    bool fail_header_write = false;
    bool fail_next_write = false;
    bool short_io = false;
    int write_stage = 0;
    int read_stage = 0;
    int write_calls = 0;
    int read_calls = 0;
    int failed_write_fd = -1;
    int closed_fd = -1;
    int replacement_fd = -1;
    std::string fail_unlink_path;
};

fault_state faults;
int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << ':' << __LINE__ \
                      << ": CHECK failed: " #condition << '\n'; \
            ++failures; \
        } \
    } while (false)

struct reset_faults {
    reset_faults() { faults = {}; }
    ~reset_faults() {
        // A replacement descriptor is intentionally owned by the test, not
        // by the old file_handle whose destructor must leave it alone.
        if (faults.replacement_fd != -1)
            __real_close(faults.replacement_fd);
        faults = {};
    }
};

struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "idbs_failure_XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* created = ::mkdtemp(buffer.data());
        if (!created)
            throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~temp_dir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

template<class F>
db::db_error expect_error(db::error_code expected, F&& operation)
{
    try {
        operation();
    }
    catch (const db::db_error& error) {
        CHECK(error.code() == expected);
        return error;
    }
    throw std::runtime_error("operation did not throw db_error");
}

int open_raw(const std::filesystem::path& path)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1)
        throw std::runtime_error("cannot create test file");
    return fd;
}

void check_fd_closed(int fd)
{
    errno = 0;
    CHECK(::fcntl(fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
}

void test_close_error_does_not_close_reused_fd(const temp_dir& tmp)
{
    reset_faults reset;
    const int fd = open_raw(tmp.path / "close_reuse.db");
    {
        db::file_handle handle(1, 11, fd);
        faults.close_error = true;
        const auto error = expect_error(db::error_code::io_error, [&] { handle.close(); });
        CHECK(error.context().os_error == EIO);
        CHECK(!handle.is_open());
        CHECK(faults.closed_fd == fd);
        CHECK(faults.replacement_fd == fd);
        CHECK(::fcntl(fd, F_GETFD) != -1);
        // Explicit repeat and destruction must both be harmless.
        handle.close();
    }
    CHECK(::fcntl(fd, F_GETFD) != -1);
}

void test_manager_unregisters_after_close_error(const temp_dir& tmp)
{
    reset_faults reset;
    const auto path = tmp.path / "manager_close.db";
    db::file_manager manager;
    manager.create_file(path.string());
    const auto id = manager.open_file(path.string());
    faults.close_error = true;
    const auto error = expect_error(db::error_code::io_error, [&] { manager.close_file(id); });
    CHECK(error.context().os_error == EIO);
    CHECK(error.context().path == path.string());
    CHECK(error.context().object_id == id);
    expect_error(db::error_code::invalid_file_handle, [&] { manager.get_file(id); });
    expect_error(db::error_code::invalid_file_handle, [&] { manager.close_file(id); });
    CHECK(::fcntl(faults.replacement_fd, F_GETFD) != -1);
    const auto reopened = manager.open_file(path.string());
    manager.close_file(reopened);
    CHECK(::fcntl(faults.replacement_fd, F_GETFD) != -1);
}

void test_header_write_failure_allows_explicit_retry(const temp_dir& tmp)
{
    reset_faults reset;
    const auto path = tmp.path / "retry_header.db";
    db::file_manager manager;
    manager.create_file(path.string());
    const auto id = manager.open_file(path.string());
    auto& handle = manager.get_file(id);
    CHECK(handle.allocate_page() == 1);
    faults.fail_header_write = true;
    const auto error = expect_error(db::error_code::io_error, [&] { manager.close_file(id); });
    CHECK(error.context().os_error == ENOSPC);
    CHECK(error.context().object_id == id);
    CHECK(error.context().path == path.string());
    CHECK(handle.is_open());
    CHECK(&manager.get_file(id) == &handle);
    CHECK(::fcntl(faults.failed_write_fd, F_GETFD) != -1);
    faults.fail_header_write = false;
    manager.close_file(id);
    const auto reopened = manager.open_file(path.string());
    CHECK(manager.get_file(reopened).get_num_pages() == 2);
    manager.close_file(reopened);
}

void test_destructor_closes_fd_after_header_write_failure(const temp_dir& tmp)
{
    reset_faults reset;
    const int fd = open_raw(tmp.path / "destructor_header.db");
    {
        db::file_handle handle(1, 12, fd);
        CHECK(handle.allocate_page() == 1);
        faults.fail_header_write = true;
    }
    CHECK(faults.failed_write_fd == fd);
    check_fd_closed(fd);
}

void test_create_write_failure_removes_only_new_file(const temp_dir& tmp)
{
    reset_faults reset;
    db::file_manager manager;
    const auto path = tmp.path / "create_write.db";
    faults.fail_next_write = true;
    const auto error = expect_error(db::error_code::io_error, [&] { manager.create_file(path.string()); });
    CHECK(error.context().os_error == ENOSPC);
    CHECK(error.context().path == path.string());
    CHECK(!error.context().cleanup_os_error.has_value());
    CHECK(!std::filesystem::exists(path));
    check_fd_closed(faults.failed_write_fd);

    // Retry succeeds; a later rejected create must preserve that real file.
    manager.create_file(path.string());
    const auto id = manager.open_file(path.string());
    db::page saved{};
    saved.fill(std::byte{0x5a});
    manager.get_file(id).append_page(saved);
    manager.close_file(id);
    expect_error(db::error_code::object_exists, [&] { manager.create_file(path.string()); });
    const auto reopened = manager.open_file(path.string());
    db::page actual{};
    manager.get_file(reopened).read_page(1, actual);
    CHECK(actual == saved);
    manager.close_file(reopened);
}

void test_create_close_failure_cleans_path_without_retry(const temp_dir& tmp)
{
    reset_faults reset;
    db::file_manager manager;
    const auto path = tmp.path / "create_close.db";
    faults.close_error = true;
    const auto error = expect_error(db::error_code::io_error, [&] { manager.create_file(path.string()); });
    CHECK(error.context().os_error == EIO);
    CHECK(error.context().path == path.string());
    CHECK(!std::filesystem::exists(path));
    CHECK(faults.replacement_fd != -1);
    CHECK(::fcntl(faults.replacement_fd, F_GETFD) != -1);
    manager.create_file(path.string());
}

void test_cleanup_error_preserves_original_failure(const temp_dir& tmp)
{
    reset_faults reset;
    db::file_manager manager;
    const auto path = tmp.path / "failed_cleanup.db";
    faults.fail_next_write = true;
    faults.fail_unlink_path = path.string();
    const auto error = expect_error(db::error_code::io_error, [&] { manager.create_file(path.string()); });
    CHECK(error.context().os_error == ENOSPC);
    CHECK(error.context().cleanup_os_error == EACCES);
    CHECK(error.context().path == path.string());
    CHECK(std::filesystem::exists(path));
    check_fd_closed(faults.failed_write_fd);
    faults.fail_unlink_path.clear();
    manager.remove_file(path.string());
}

void test_short_io_and_eintr_preserve_bytes(const temp_dir& tmp)
{
    reset_faults reset;
    db::fd_guard file(open_raw(tmp.path / "short_io.db"));
    db::page expected{};
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<std::byte>((i * 17 + 3) & 0xff);
    faults.short_io = true;
    db::safe_write(file.get(), expected.data(), expected.size(), 0);
    db::page actual{};
    db::safe_read(file.get(), actual.data(), actual.size(), 0);
    CHECK(faults.write_calls > 2 && faults.read_calls > 2);
    CHECK(expected == actual);
    faults.short_io = false;
    expect_error(db::error_code::invalid_format, [&] {
        db::safe_read(file.get(), actual.data(), actual.size(), db::page_size);
    });
}

void test_whole_page_offset_limit_precedes_syscall(const temp_dir& tmp)
{
    reset_faults reset;
    const auto max_pages = static_cast<db::page_id>(std::numeric_limits<off_t>::max()) / db::page_size;
    db::file_handle handle(max_pages, 13, open_raw(tmp.path / "offset_limit.db"));
    const auto original_count = handle.get_num_pages();
    const auto writes = faults.write_calls;
    expect_error(db::error_code::invalid_page, [&] { handle.allocate_page(); });
    db::page bytes{};
    expect_error(db::error_code::invalid_page, [&] { handle.append_page(bytes); });
    CHECK(handle.get_num_pages() == original_count);
    CHECK(faults.write_calls == writes);

    // An artificially large in-memory page count must not bypass the full
    // page offset limit on read/write either. No huge file is created.
    db::file_handle too_large(max_pages + 1, 14, open_raw(tmp.path / "offset_read.db"));
    const auto reads = faults.read_calls;
    expect_error(db::error_code::invalid_page, [&] { too_large.read_page(max_pages, bytes); });
    expect_error(db::error_code::invalid_page, [&] { too_large.write_page(max_pages, bytes); });
    CHECK(faults.read_calls == reads && faults.write_calls == writes);
}

} // namespace

extern "C" int __wrap_close(int fd)
{
    if (!faults.close_error)
        return __real_close(fd);
    faults.close_error = false;
    const int result = __real_close(fd);
    if (result != 0)
        return result;
    faults.closed_fd = fd;
    // Reserve the just-released descriptor before old-object cleanup can
    // mistakenly close it again. This makes the regression deterministic.
    int replacement = ::open("/dev/null", O_RDWR);
    if (replacement != -1 && replacement != fd) {
        if (::dup2(replacement, fd) == fd) {
            __real_close(replacement);
            replacement = fd;
        }
    }
    faults.replacement_fd = replacement;
    errno = EIO;
    return -1;
}

extern "C" ssize_t __wrap_pwrite(int fd, const void* buffer, size_t count, off_t offset)
{
    ++faults.write_calls;
    if (faults.fail_next_write || (faults.fail_header_write && offset == 16 && count == 8)) {
        faults.fail_next_write = false;
        faults.failed_write_fd = fd;
        errno = ENOSPC;
        return -1;
    }
    if (faults.short_io) {
        if (faults.write_stage++ == 0) {
            errno = EINTR;
            return -1;
        }
        count = std::min(count, std::size_t{37});
    }
    return __real_pwrite(fd, buffer, count, offset);
}

extern "C" ssize_t __wrap_pread(int fd, void* buffer, size_t count, off_t offset)
{
    ++faults.read_calls;
    if (faults.short_io) {
        if (faults.read_stage++ == 0) {
            errno = EINTR;
            return -1;
        }
        count = std::min(count, std::size_t{29});
    }
    return __real_pread(fd, buffer, count, offset);
}

extern "C" int __wrap_unlink(const char* path)
{
    if (faults.fail_unlink_path == path) {
        errno = EACCES;
        return -1;
    }
    return __real_unlink(path);
}

int main()
{
    try {
        temp_dir tmp;
        test_close_error_does_not_close_reused_fd(tmp);
        test_manager_unregisters_after_close_error(tmp);
        test_header_write_failure_allows_explicit_retry(tmp);
        test_destructor_closes_fd_after_header_write_failure(tmp);
        test_create_write_failure_removes_only_new_file(tmp);
        test_create_close_failure_cleans_path_without_retry(tmp);
        test_cleanup_error_preserves_original_failure(tmp);
        test_short_io_and_eintr_preserve_bytes(tmp);
        test_whole_page_offset_limit_precedes_syscall(tmp);
    }
    catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "storage fault tests passed (9 groups)\n";
    return 0;
}
