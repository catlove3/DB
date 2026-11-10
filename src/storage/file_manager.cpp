#include "db/storage/file_manager.h"
#include <unistd.h>      // close(), pread(), pwrite()
#include <fcntl.h>       // open(), O_RDWR
#include <cerrno>        // errno
#include <cstring>       // memset, strerror
#include <algorithm>
#include <filesystem>
#include <functional>
#include <limits>
#include <sys/stat.h>

namespace db
{

static constexpr page empty_page = page();
constexpr file_header fh(magic,ver,page_size,1);
constexpr off_t num_pages_offset = sizeof(file_header::magic_number) + sizeof(file_header::version) + sizeof(file_header::page_size);

namespace {

/*
    HELPERS : FILE ERROR CONTEXT
    docs:
        Build owning diagnostic values without changing files or caller state.
        String allocation failures propagate; constructing an error does not undo I/O.

    methods:
        path_context    : Copy operation/path; do not check whether the path exists.
        throw_os_error  : Throw the chosen code with saved errno; preserve other context fields.
        path_error_code : Map EEXIST to object_exists, ENOENT/ENOTDIR to object_not_found,
                          and other saved error numbers to io_error.
        validate_io     : Require a non-null buffer for nonzero size, a nonnegative offset,
                          and a representable offset + size; otherwise throw invalid_argument.

    errors:
        Buffer extent/accessibility and valid operation strings remain caller preconditions.
        Zero-size I/O permits nullptr. Validation performs no I/O and never dereferences buffers.
*/
error_context path_context(const std::string& operation, const std::string& path)
{
    error_context result;
    result.operation = operation;
    result.path = path;
    return result;
}

[[noreturn]] void throw_os_error(const std::string& operation, int saved_errno,
                                error_context context = {},
                                error_code code = error_code::io_error)
{
    context.operation = operation;
    context.os_error = saved_errno;
    throw db_error(code, operation + ": " + std::strerror(saved_errno), std::move(context));
}

error_code path_error_code(int saved_errno)
{
    if (saved_errno == EEXIST)
        return error_code::object_exists;
    if (saved_errno == ENOENT || saved_errno == ENOTDIR)
        return error_code::object_not_found;
    return error_code::io_error;
}

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


/*
    FUNCTIONS : FILE I/O AND BYTE ENCODING
    docs:
        Borrow descriptors and caller-owned buffers; do not close descriptors or change their offsets.
        Byte helpers use little-endian encoding and require accessible buffers of 4 or 8 bytes.

    methods:
        normalize_path : Resolve an absolute, weakly canonical path; missing suffixes are allowed.
        safe_read      : Read exactly n bytes with pread, retrying EINTR and short reads.
        safe_write     : Write exactly n bytes with pwrite, retrying EINTR and short writes.
        read_u32/u64   : Decode an unsigned value without validation, allocation or I/O.
        write_u32/u64  : Write only the bytes of the requested unsigned value.

    errors:
        Empty paths or invalid I/O extents -> invalid_argument; early EOF -> invalid_format.
        Path resolution, system I/O failures and zero-progress writes -> io_error.
        Zero-size reads/writes issue no syscall. I/O failures may leave a partially changed buffer/file.
        Byte helpers do not validate pointers; path normalization is not a filesystem lock.
        Successful writes do not provide rollback, fsync or crash recovery.
*/
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


/*
    CLASS : FD GUARD
    docs:
        Own one temporary descriptor; -1 means empty. Copying and moving are disabled.
        After adoption, only this guard or the recipient of release() may close the descriptor.

    methods:
        constructor : Adopt a descriptor without validation or I/O.
        get         : Borrow the descriptor; ownership stays with the guard.
        release     : Return the descriptor and become empty; repeated calls return -1.
        destructor  : Close a nonempty descriptor once, without retries or exceptions.

    errors:
        Destruction ignores close failures. Use an explicit checked close when errors must be reported.
*/
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




/*
    STRUCT : FILE HEADER
    docs:
        Describe page 0 of a page file; the page count includes page 0 itself.
        The constexpr constructor stores fields without validation; its defaults are not a valid header.

    layout:
        0 : magic_number -> uint32_t, 0x114514
        4 : version      -> uint32_t, 1
        8 : page_size    -> uint64_t, 8192
        16: num_pages    -> uint64_t, at least 1 and representable as a complete file extent

    methods:
        write_header : Encode the first 24 bytes, leaving the rest of the page unchanged.
        read_header  : Decode and validate all four fields, returning an independent value.

    errors:
        Bad magic, version, page size or page count -> invalid_format.
        These methods do not check the actual file length or issue I/O.
*/
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



/*
    CLASS : FILE HANDLE
    docs:
        Own one descriptor, its logical page count and optional diagnostic path; not thread-safe.
        The default constructor creates a closed handle. The parameterized constructor adopts the
        descriptor and trusts supplied metadata without opening, validating or registering the file.
        Manager-owned handles are borrowed by callers; cached files must be accessed/closed through bpm.

    methods:
        is_open       : Report whether this object owns a descriptor, without probing the OS.
        read_page     : Read an existing page, including page 0, into a borrowed destination.
        write_page    : Write an existing data page; page 0 is reserved for the file manager.
        allocate_page : Append a zero-filled page and return its previous logical page count as the ID.
        append_page   : Append the supplied page; increase the count only after a complete write.
        close         : Save a dirty page count, then close; already-closed handles are harmless.
        get_id        : Return the assigned ID, even after closure.
        get_num_pages : Return the in-memory count, including page 0; do not inspect file length.
        destructor    : Best-effort save/close; still close once if saving the header fails.

    helpers:
        context     : Own operation, file ID, optional page ID and a nonempty diagnostic path.
        calc_offset : Require an open handle and a representable whole-page offset; normal access
                      also requires idx < num_pages. Append callers supply exactly num_pages.

    errors:
        Closed handles -> invalid_file_handle; bad page extents/page-0 writes -> invalid_page.
        Truncated reads -> invalid_format; system failures -> io_error; allocations may propagate.
        Failed append preserves the in-memory count but can leave partial trailing bytes on disk.
        Header-write failure during close keeps the handle open and dirty. Once close is called on
        the descriptor, ownership is cleared even if it fails; that descriptor is never retried.
        Page writes do not invalidate cached copies. There is no rollback, fsync or crash recovery.
*/
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


/*
    CLASS : FILE MANAGER
    docs:
        Own file handles and three unordered lookup tables: file ID, canonical path and device/inode.
        Reject repeated opens through either the same path or a hard-link alias; no sorted order is used.
        Operate in one thread without a cross-process lock. The manager must outlive its buffer pools.
        External code must not rename, replace, truncate or directly close registered files.

    methods:
        constructor : Start with no files and next file ID 0; create no directories.
        create_file : Exclusively create a one-page file with a valid header; do not register it.
        open_file   : Open read/write, validate the header and minimum length, then register all keys.
        close_file  : Save/close a registered handle and remove its three lookup entries.
        remove_file : Unlink a normalized path; reject an open file, including hard-link aliases.
        get_file    : Borrow a registered handle without opening it or probing its descriptor.
        destructor  : Destroy handles with best-effort cleanup; all borrowed references expire.

    helpers:
        physical_identity_hash : Mix device and inode hashes; equality still compares both fields.
        unregister_file        : Remove all lookup entries; absent IDs are harmless.

    errors:
        Empty paths -> invalid_argument; missing paths -> object_not_found; existing create targets
        -> object_exists; duplicate/open aliases -> resource_busy; exhausted IDs -> resource_limit.
        Nonregular files, malformed headers and short files -> invalid_format; OS failures -> io_error.
        IDs increase without reuse; the maximum uint64_t value is not allocated. Trailing bytes are allowed.
        Failed open rolls back registration and closes its temporary descriptor without repairing the file.
        Failed create removes only its newly created file; db_error retains a secondary unlink error
        in cleanup_os_error. Allocation failures propagate and temporary close failures are ignored.
        Failed header save on close preserves registration; after the close syscall, success or failure
        removes registration and invalidates borrowed handles. Repeated close reports invalid_file_handle.
        Cached files must close through bpm. Removing files does not recursively remove directories,
        automatically close handles, or guarantee crash durability; symlinks resolve to their targets.
*/
std::size_t file_manager::physical_identity_hash::operator()(const physical_identity& identity)const noexcept
{
    std::size_t device = std::hash<dev_t>{}(identity.first);
    std::size_t inode = std::hash<ino_t>{}(identity.second);
    return device ^ (inode + std::size_t{0x9e3779b9} + (device << 6) + (device >> 2));
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
    // Clean up only the file created by this O_EXCL call: close once, then unlink.
    // Return unlink's errno; a cleanup failure can leave the new file on disk.
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
