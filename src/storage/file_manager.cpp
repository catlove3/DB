#include "db/storage/file_manager.h"
#include <unistd.h>      // close(), pread(), pwrite()
#include <fcntl.h>       // open(), O_RDWR 等（如果这里不用open可以暂时不需要）
#include <cerrno>        // errno
#include <cstring>       // memset, strerror
#include <stdexcept>     // runtime_error
#include <filesystem>
#include <limits>
#include <memory>
#include <sys/stat.h>

namespace db
{

static constexpr page empty_page = page();
static constexpr mode_t FILE_MODE = 0644;
constexpr uint32_t magic = 0x114514;
constexpr uint32_t ver = 1;
constexpr file_header fh(magic,ver,page_size,1);
constexpr off_t num_pages_offset = sizeof(file_header::magic_number) + sizeof(file_header::version) + sizeof(file_header::page_size);


std::string normalize_path(const std::string & path)
{
    std::error_code ec;
    std::filesystem::path absolute_path = std::filesystem::absolute(path,ec);
    if(ec) throw std::filesystem::filesystem_error("failed to make path absolute", path, ec);

    std::filesystem::path p = std::filesystem::weakly_canonical(absolute_path,ec);
    if(ec) throw std::filesystem::filesystem_error("failed to normalize path", path, ec);
    return p.string();
}



void safe_read(int os_id,void * dest,size_t n,off_t offset)
{
    uint8_t * dst = (uint8_t *)dest;
    size_t done = 0;
    while(done < n)
    {
        ssize_t flag = ::pread(os_id,dst+done,n-done,offset+(off_t)done);
        if(flag < 0)
        {
            if(errno == EINTR)continue;
            throw std::runtime_error(std::string("pread: ") + std::strerror(errno));
        }
        if(flag == 0) throw std::runtime_error("unexpected EOF");
        done += (size_t)flag;
    }
}

void safe_write(int os_id,const void*srcc,size_t n,off_t offset)
{
    uint8_t * src = (uint8_t*)srcc;
    size_t done = 0;
    while(done < n)
    {
        ssize_t flag = ::pwrite(os_id,src + done,n-done,offset+(off_t)done);
        if(flag < 0)
        {
            if(errno == EINTR)continue;
            throw std::runtime_error(std::string("pwrite: ") + std::strerror(errno));
        }
        if(flag == 0) throw std::runtime_error("we write zero bytes");
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
        throw std::runtime_error("invalid magic number.");
    ptr+= sizeof(header.magic_number);

    header.version = read_u32(ptr);
    ptr += sizeof(header.version);

    header.page_size = read_u64(ptr);
    if(header.page_size != db::page_size)
        throw std::runtime_error("page size mismatch");
    ptr += sizeof(header.page_size);

    header.num_pages = read_u64(ptr);

    return header;
}



file_handle::file_handle():num_pages(0),id(0),os_id(-1),dirty(false){}


file_handle::file_handle(page_id npg,file_id fid,int oid):num_pages(npg),id(fid),os_id(oid),dirty(false){}


file_handle::~file_handle()noexcept
{
    try{
        if(is_open())
            close();
    }
    catch(std::exception & e)
    {
    }
}


bool file_handle::is_open()const
{
    return os_id != -1; 
}


off_t file_handle::calc_offset(page_id idx,bool append_back)
{
    if(!is_open())
        throw std::runtime_error("this handle hasn't been assigned to any file yet.");

    if(!append_back && idx >= num_pages)
        throw std::runtime_error("invalid page id.");

    const uint64_t max_pages = (uint64_t)std::numeric_limits<off_t>::max() / page_size;
    if(idx > max_pages)
        throw std::runtime_error("page id is too large.");

    return (off_t)idx * (off_t)page_size;
}


void file_handle::read_page(page_id idx,page& dest)
{
    off_t offset = calc_offset(idx);
    safe_read(os_id,dest.data(),page_size,offset);
}


void file_handle::write_page(page_id idx,const page & src)
{
    off_t offset = calc_offset(idx);
    safe_write(os_id,(void*)src.data(),page_size,offset);
}


page_id file_handle::allocate_page()
{
    off_t offset = calc_offset(num_pages,true);
    safe_write(os_id,(void*)empty_page.data(),page_size,offset);
    dirty = true;
    return num_pages++;
}


void file_handle::close()
{
    if(!is_open())
        return;

    if(dirty)
    {
        std::byte buf[8];
        write_u64(buf,num_pages);
        safe_write(os_id,buf,sizeof(buf),num_pages_offset);
    }

    if(::close(os_id) == -1)
        throw std::runtime_error("fail to close file.");

    os_id = -1;
}


void file_handle::append_page(const page & src)
{
    off_t offset = calc_offset(num_pages,true);
    safe_write(os_id,(void*)src.data(),page_size,offset);
    ++num_pages;
    dirty = true;
}

file_id file_handle::get_id()const noexcept{
    return id;
}


file_manager::file_manager():next(0){}


void file_manager::create_file(const std::string & path)
{
    std::string normalized_path = normalize_path(path);
    int os_id = ::open(normalized_path.c_str(),O_CREAT | O_EXCL | O_RDWR,FILE_MODE);
    if(os_id == -1)
        throw std::runtime_error("attempt to creat file that already exists.");
    page p{};
    fh.write_header(p);
    try{
        safe_write(os_id,p.data(),page_size,0);
    }
    catch(...){
        ::close(os_id);
        throw std::runtime_error("unable to write header into file after creating file.");
    }

    
    if(::close(os_id) == -1)
        throw std::runtime_error("unable to close file after creating file.");
}


file_handle* file_manager::open_file(const std::string & maybe_un_normalized_path)
{
    std::string path = normalize_path(maybe_un_normalized_path);
    if(filenames.find(path) != filenames.end())
        throw std::runtime_error("attempt to open the same file twice.");
    
    int os_id = ::open(path.c_str(),O_RDWR);
    if(os_id == -1)
        throw std::runtime_error("fail to open file.");
    
    fd_guard guard = fd_guard(os_id);
    page header_page{};
    safe_read(guard.get(),header_page.data(),page_size,0);
    
    
    file_header header = file_header::read_header(header_page);
    if(header.version != ver)
        throw std::runtime_error("invalid version of files");

    const uint64_t max_pages = (uint64_t)std::numeric_limits<off_t>::max() / page_size;
    if(header.num_pages == 0 || header.num_pages > max_pages)
        throw std::runtime_error("invalid page count.");

    struct stat info{};
    if(::fstat(guard.get(),&info) == -1)
        throw std::runtime_error(std::string("fstat: ") + std::strerror(errno));
    if(info.st_size < 0 || (uint64_t)info.st_size / page_size < header.num_pages)
        throw std::runtime_error("file is smaller than the number of pages in its header.");

    file_id id = next;
    std::unique_ptr<file_handle> fh(new file_handle(header.num_pages,id,guard.get()));
    guard.release();

    try{
        file_handles.emplace(id,fh.get());
        filenames.emplace(path,id);
        idx2name.emplace(id,path);
    }
    catch(...){
        file_handles.erase(id);
        filenames.erase(path);
        idx2name.erase(id);
        throw;
    }

    next = id + 1;
    return fh.release();
}


void file_manager::close_file(file_id id)
{
    auto it = file_handles.find(id);
    if(it == file_handles.end())
        throw std::runtime_error("attempt to close unopened or double close file.");
    
    file_handle * fh = it->second;
    std::string name = idx2name[id];

    fh->close();
    delete fh;

    file_handles.erase(it);
    filenames.erase(name);
    idx2name.erase(id);
}

file_manager::~file_manager()
{
    for(auto& it:file_handles)
        delete it.second;
    file_handles.clear();
    filenames.clear();
    idx2name.clear();
}


void file_manager::remove_file(const std::string & path)
{
    std::string normalized_path = normalize_path(path);
    auto it = filenames.find(normalized_path);
    if(it != filenames.end())
        throw std::runtime_error("can't remove opened files");
    
    if(::unlink(normalized_path.c_str()) == -1)
        throw std::runtime_error("fail to remove file.");
}






};