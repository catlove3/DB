#include "db/storage/file_manager.h"
#include <unistd.h>      // close(), pread(), pwrite()
#include <fcntl.h>       // open(), O_RDWR 等（如果这里不用open可以暂时不需要）
#include <cerrno>        // errno
#include <cstring>       // memset, strerror
#include <algorithm>
#include <filesystem>
#include <limits>
#include <sys/stat.h>

namespace db
{

static constexpr page empty_page = page();
constexpr file_header fh(magic,ver,page_size,1);
constexpr off_t num_pages_offset = sizeof(file_header::magic_number) + sizeof(file_header::version) + sizeof(file_header::page_size);

namespace {

// 按值复制操作名和路径到独立错误上下文，不检查路径是否存在；不修改输入或文件。
// 字符串复制可能抛分配异常，尚未完成的上下文不会向调用者发布。
error_context path_context(const std::string& operation, const std::string& path)
{
    error_context result;
    result.operation = operation;
    result.path = path;
    return result;
}

/**
 * 把调用方在失败现场保存的 errno 转成指定 code 的 db_error，默认 io_error。
 * context 按值接收，覆盖其中 operation/os_error，保留路径、对象和页等其他字段。
 * 始终抛出；错误文本/上下文分配失败可改为抛标准分配异常，不执行清理或 I/O。
 * 因此它不撤销此前系统调用造成的状态，调用方仍负责关闭和失败边界判断。
 */
[[noreturn]] void throw_os_error(const std::string& operation, int saved_errno,
                                error_context context = {},
                                error_code code = error_code::io_error)
{
    context.operation = operation;
    context.os_error = saved_errno;
    throw db_error(code, operation + ": " + std::strerror(saved_errno), std::move(context));
}

// 将已保存 errno 映射到路径类错误：EEXIST→object_exists，
// ENOENT/ENOTDIR→object_not_found，其余→io_error；不读当前 errno，不修改状态。
error_code path_error_code(int saved_errno)
{
    if (saved_errno == EEXIST)
        return error_code::object_exists;
    if (saved_errno == ENOENT || saved_errno == ENOTDIR)
        return error_code::object_not_found;
    return error_code::io_error;
}

/**
 * 仅验证 I/O 参数形状：非零长度需非空 buffer，offset 非负，offset+size 可由 off_t 表示。
 * buffer 的实际可访问字节数和读写权限仍由调用方保证；operation 必须为有效 C 字符串。
 * 不合法时抛 invalid_argument（构造异常可能分配失败），合法时直接返回。
 * 不解引用 buffer，不发起 I/O，不改变内存或磁盘；size=0 允许 buffer=nullptr。
 */
void validate_io(const void* buffer, std::size_t size, off_t offset,
                 const char* operation)
{
    if ((size != 0 && buffer == nullptr) || offset < 0 ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max() - offset)) {
        error_context context;
        context.operation = operation;
        throw db_error(error_code::invalid_argument, "invalid I/O buffer or extent",
                       std::move(context));
    }
}

} // namespace


std::string normalize_path(const std::string & path)
{
    if (path.empty())
        throw db_error(error_code::invalid_argument, "empty file path",
                       path_context("normalize_path", path));
    std::error_code ec;
    std::filesystem::path absolute_path = std::filesystem::absolute(path,ec);
    if(ec) throw_os_error("absolute_path", ec.value(), path_context("absolute_path", path));

    std::filesystem::path p = std::filesystem::weakly_canonical(absolute_path,ec);
    if(ec) throw_os_error("normalize_path", ec.value(), path_context("normalize_path", path));
    return p.string();
}



void safe_read(int os_id,void * dest,size_t n,off_t offset)
{
    validate_io(dest, n, offset, "pread");
    uint8_t * dst = (uint8_t *)dest;
    size_t done = 0;
    while(done < n)
    {
        const size_t remaining = std::min(n - done, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        ssize_t flag = ::pread(os_id,dst+done,remaining,offset+(off_t)done);
        if(flag < 0)
        {
            const int saved_errno = errno;
            if(saved_errno == EINTR)continue;
            throw_os_error("pread", saved_errno);
        }
        if(flag == 0) {
            error_context context;
            context.operation = "pread";
            throw db_error(error_code::invalid_format, "unexpected EOF", std::move(context));
        }
        done += (size_t)flag;
    }
}

void safe_write(int os_id,const void*srcc,size_t n,off_t offset)
{
    validate_io(srcc, n, offset, "pwrite");
    const uint8_t * src = static_cast<const uint8_t*>(srcc);
    size_t done = 0;
    while(done < n)
    {
        const size_t remaining = std::min(n - done, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        ssize_t flag = ::pwrite(os_id,src + done,remaining,offset+(off_t)done);
        if(flag < 0)
        {
            const int saved_errno = errno;
            if(saved_errno == EINTR)continue;
            throw_os_error("pwrite", saved_errno);
        }
        if(flag == 0) {
            error_context context;
            context.operation = "pwrite";
            throw db_error(error_code::io_error, "pwrite made no progress", std::move(context));
        }
        done += (size_t)flag;
    }
}




uint32_t read_u32(const void * ssrc)noexcept
{
    const uint8_t * src = (const uint8_t *)ssrc;
    return
        (uint32_t(src[0])      ) |
        (uint32_t(src[1]) << 8 ) |
        (uint32_t(src[2]) << 16) |
        (uint32_t(src[3]) << 24);
}

void write_u32(void* dest, uint32_t value)noexcept
{
    uint8_t * dst = (uint8_t * )dest;

    dst[0] = (value & 0xff);
    dst[1] = ((value >> 8) & 0xff);
    dst[2] = ((value >> 16) & 0xff);
    dst[3] = ((value >> 24) & 0xff);
}

void write_u64(void* dest, uint64_t value)noexcept
{
    uint8_t * dst = (uint8_t *)dest;
    for(int i = 0; i < 8; i++)
        dst[i] = ((value >> (8 * i)) & 0xff);
}

uint64_t read_u64(const void* ssrc)noexcept
{
    uint64_t value = 0;
    const uint8_t * src = (const uint8_t * )ssrc;
    for(int i = 0; i < 8; i++)
        value |= uint64_t(src[i])<< (8 * i);
    
    return value;
}


fd_guard::fd_guard(int fd)noexcept:fd_(fd){}

int fd_guard::get()const noexcept{
    return fd_;
}

int fd_guard::release()noexcept{
    int meow = -1;
    std::swap(meow,fd_);
    return meow;
}

fd_guard::~fd_guard()noexcept
{
    if(fd_ != -1)
        ::close(fd_);
}




void file_header::write_header(page& p) const
{
    write_u32(p.data()+0, magic_number);
    write_u32(p.data()+4, version);
    write_u64(p.data()+8, page_size);
    write_u64(p.data()+16, num_pages);
}

file_header file_header::read_header(const page& p)
{
    const void* pointer = p.data();
    const uint8_t * ptr = (const uint8_t *)pointer;
    file_header header;

    header.magic_number = read_u32(ptr);
    if(header.magic_number != magic)
        throw db_error(error_code::invalid_format, "invalid magic number");
    ptr+= sizeof(header.magic_number);

    header.version = read_u32(ptr);
    if(header.version != ver)
        throw db_error(error_code::invalid_format, "unsupported file version");
    ptr += sizeof(header.version);

    header.page_size = read_u64(ptr);
    if(header.page_size != db::page_size)
        throw db_error(error_code::invalid_format, "page size mismatch");
    ptr += sizeof(header.page_size);

    header.num_pages = read_u64(ptr);
    const auto max_pages = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / db::page_size;
    if(header.num_pages == 0 || header.num_pages > max_pages)
        throw db_error(error_code::invalid_format, "invalid page count");

    return header;
}



file_handle::file_handle():num_pages(0),id(0),os_id(-1),dirty(false){}


file_handle::file_handle(page_id npg,file_id fid,int oid,std::string path)
    :num_pages(npg),id(fid),os_id(oid),dirty(false),path_(std::move(path)){}

error_context file_handle::context(const std::string& operation,
                                   std::optional<page_id> pid) const
{
    error_context result;
    result.operation = operation;
    if (!path_.empty())
        result.path = path_;
    result.object_id = id;
    result.page = pid;
    return result;
}


file_handle::~file_handle()noexcept
{
    try{
        if(is_open())
            close();
    }
    catch(...)
    {
        // A failed header write must not prevent final descriptor release.
        // close() clears os_id before the system call, so an already-submitted
        // close (including EIO/EINTR) is never retried here.
        if (os_id != -1) {
            const int owned_fd = os_id;
            os_id = -1;
            ::close(owned_fd);
        }
    }
}


bool file_handle::is_open()const
{
    return os_id != -1; 
}


off_t file_handle::calc_offset(page_id idx,bool append_back)const
{
    if(!is_open())
        throw db_error(error_code::invalid_file_handle, "file handle is closed", context("page_offset", idx));

    if(!append_back && idx >= num_pages)
        throw db_error(error_code::invalid_page, "page is outside the file", context("page_offset", idx));

    const uint64_t max_pages = (uint64_t)std::numeric_limits<off_t>::max() / page_size;
    // The entire page, not merely its starting offset, must fit in off_t.
    if(idx >= max_pages)
        throw db_error(error_code::invalid_page, "page extent exceeds the file offset range", context("page_offset", idx));

    return (off_t)idx * (off_t)page_size;
}


void file_handle::read_page(page_id idx,page& dest)const
{
    off_t offset = calc_offset(idx);
    try {
        safe_read(os_id,dest.data(),page_size,offset);
    }
    catch (const db_error& error) {
        auto details = context("read_page", idx);
        details.os_error = error.context().os_error;
        throw db_error(error.code(), error.what(), std::move(details));
    }
}


void file_handle::write_page(page_id idx,const page & src)
{
    off_t offset = calc_offset(idx);
    if (idx == 0)
        throw db_error(error_code::invalid_page, "page zero is owned by the file manager", context("write_page", idx));
    try {
        safe_write(os_id,src.data(),page_size,offset);
    }
    catch (const db_error& error) {
        auto details = context("write_page", idx);
        details.os_error = error.context().os_error;
        throw db_error(error.code(), error.what(), std::move(details));
    }
}


page_id file_handle::allocate_page()
{
    const page_id result = num_pages;
    append_page(empty_page);
    return result;
}


void file_handle::close()
{
    if(!is_open())
        return;

    if(dirty)
    {
        std::byte buf[8];
        write_u64(buf,num_pages);
        try {
            safe_write(os_id,buf,sizeof(buf),num_pages_offset);
        }
        catch (const db_error& error) {
            auto details = context("save_file_header", 0);
            details.os_error = error.context().os_error;
            throw db_error(error.code(), error.what(), std::move(details));
        }
        dirty = false;
    }

    const int owned_fd = os_id;
    os_id = -1;
    if(::close(owned_fd) == -1) {
        const int saved_errno = errno;
        throw_os_error("close_file", saved_errno, context("close_file"));
    }
}


void file_handle::append_page(const page & src)
{
    off_t offset = calc_offset(num_pages,true);
    try {
        safe_write(os_id,src.data(),page_size,offset);
    }
    catch (const db_error& error) {
        auto details = context("append_page", num_pages);
        details.os_error = error.context().os_error;
        throw db_error(error.code(), error.what(), std::move(details));
    }
    ++num_pages;
    dirty = true;
}

file_id file_handle::get_id()const noexcept{
    return id;
}


file_manager::file_manager():next(0){}


void file_manager::create_file(const std::string & path)const
{
    std::string normalized_path = normalize_path(path);
    int os_id = ::open(normalized_path.c_str(),O_CREAT | O_EXCL | O_RDWR,FILE_MODE);
    if(os_id == -1) {
        const int saved_errno = errno;
        throw_os_error("create_file", saved_errno, path_context("create_file", normalized_path),
                       path_error_code(saved_errno));
    }
    fd_guard guard(os_id);
    // This manager is single-threaded and operates on its managed directory.
    // Only the path successfully created by this call is eligible for cleanup.
    // 仅用于本次 O_EXCL 成功创建后的失败清理；借用仍存活的 guard 和 normalized_path。
    // 先释放 guard 所有权并最多提交一次 close，再 unlink 本次路径；不重试系统 close。
    // 返回 0 或 unlink 的 errno；close 错误不报告。无异常，但失败可留下本次新建文件。
    const auto cleanup = [&]() noexcept -> int {
        const int owned_fd = guard.release();
        if (owned_fd != -1)
            ::close(owned_fd); // Never retry a submitted close on Linux.
        if (::unlink(normalized_path.c_str()) == -1)
            return errno;
        return 0;
    };
    try {
        page p{};
        fh.write_header(p);
        safe_write(guard.get(),p.data(),page_size,0);
        const int owned_fd = guard.release();
        if (::close(owned_fd) == -1) {
            const int saved_errno = errno;
            throw_os_error("close_created_file", saved_errno);
        }
    }
    catch (const db_error& error) {
        const int cleanup_errno = cleanup();
        auto details = error.context();
        details.path = normalized_path;
        if (cleanup_errno != 0)
            details.cleanup_os_error = cleanup_errno;
        throw db_error(error.code(), error.what(), std::move(details));
    }
    catch (...) {
        cleanup();
        throw;
    }
}


file_id file_manager::open_file(const std::string & maybe_un_normalized_path)
{
    std::string path = normalize_path(maybe_un_normalized_path);
    if(filenames.find(path) != filenames.end())
        throw db_error(error_code::resource_busy, "file is already open",
                       path_context("open_file", path));

    if (next == std::numeric_limits<file_id>::max())
        throw db_error(error_code::resource_limit, "file ID space exhausted",
                       path_context("open_file", path));
    
    int os_id = ::open(path.c_str(),O_RDWR);
    if(os_id == -1) {
        const int saved_errno = errno;
        throw_os_error("open_file", saved_errno, path_context("open_file", path),
                       path_error_code(saved_errno));
    }
    
    fd_guard guard = fd_guard(os_id);
    struct stat info{};
    if(::fstat(guard.get(),&info) == -1) {
        const int saved_errno = errno;
        throw_os_error("fstat", saved_errno, path_context("open_file", path));
    }
    if (!S_ISREG(info.st_mode))
        throw db_error(error_code::invalid_format, "page storage requires a regular file",
                       path_context("open_file", path));
    const physical_identity identity{info.st_dev, info.st_ino};
    if (identities.find(identity) != identities.end())
        throw db_error(error_code::resource_busy, "physical file is already open",
                       path_context("open_file", path));

    file_header header;
    try {
        page header_page{};
        safe_read(guard.get(),header_page.data(),page_size,0);
        header = file_header::read_header(header_page);
        if(info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) / page_size < header.num_pages)
            throw db_error(error_code::invalid_format,
                           "file is smaller than the page count in its header");
    }
    catch (const db_error& error) {
        auto details = error.context();
        details.operation = "open_file";
        details.path = path;
        details.page = 0;
        throw db_error(error.code(), error.what(), std::move(details));
    }

    file_id id = next;
    auto handle = std::make_unique<file_handle>(header.num_pages,id,guard.get(),path);
    guard.release();
    open_file_entry entry{std::move(handle), path, identity};

    try{
        file_handles.emplace(id,std::move(entry));
        filenames.emplace(path,id);
        identities.emplace(identity,id);
    }
    catch(...){
        file_handles.erase(id);
        filenames.erase(path);
        identities.erase(identity);
        throw;
    }

    next = id + 1;
    return id;
}


void file_manager::close_file(file_id id)
{
    auto it = file_handles.find(id);
    if(it == file_handles.end()) {
        error_context details;
        details.operation = "close_file";
        details.object_id = id;
        throw db_error(error_code::invalid_file_handle, "file ID is not open", std::move(details));
    }

    try {
        it->second.handle->close();
    }
    catch (...) {
        // A header-save failure retains a live descriptor for explicit retry.
        // A submitted system close invalidates the descriptor even on failure.
        if (!it->second.handle->is_open())
            unregister_file(id);
        throw;
    }
    unregister_file(id);
}

void file_manager::unregister_file(file_id id)noexcept
{
    auto it = file_handles.find(id);
    if (it == file_handles.end())
        return;
    filenames.erase(it->second.path);
    identities.erase(it->second.identity);
    file_handles.erase(it);
}

file_manager::~file_manager() = default;


void file_manager::remove_file(const std::string & path)
{
    std::string normalized_path = normalize_path(path);
    auto it = filenames.find(normalized_path);
    if(it != filenames.end())
        throw db_error(error_code::resource_busy, "cannot remove an open file",
                       path_context("remove_file", normalized_path));

    struct stat info{};
    if (::stat(normalized_path.c_str(), &info) == -1) {
        const int saved_errno = errno;
        throw_os_error("stat", saved_errno, path_context("remove_file", normalized_path),
                       path_error_code(saved_errno));
    }
    if (identities.find({info.st_dev, info.st_ino}) != identities.end())
        throw db_error(error_code::resource_busy, "cannot remove an alias of an open file",
                       path_context("remove_file", normalized_path));
    
    if(::unlink(normalized_path.c_str()) == -1) {
        const int saved_errno = errno;
        throw_os_error("unlink", saved_errno, path_context("remove_file", normalized_path),
                       path_error_code(saved_errno));
    }
}


file_handle & file_manager::get_file(file_id id)
{
    auto it = file_handles.find(id);
    if(it == file_handles.end()) {
        error_context details;
        details.operation = "get_file";
        details.object_id = id;
        throw db_error(error_code::invalid_file_handle, "file ID is not open", std::move(details));
    }
    
    return *it->second.handle;
} 



};
