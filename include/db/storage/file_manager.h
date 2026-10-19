#pragma once

#include "db/common/types.h"
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdint>
#include <sys/types.h>   // off_t

namespace db{
    uint32_t read_u32(const void * src)noexcept;
    uint64_t read_u64(const void * src)noexcept;
    void write_u32(void * dest,uint32_t num)noexcept;
    void write_u64(void * dest,uint64_t num)noexcept;
    void safe_read(int os_id,void * dest,size_t n,off_t offset);
    void safe_write(int os_id,const void*srcc,size_t n,off_t offset);
    std::string normalize_path(const std::string&path);

    class fd_guard
    {
    private:
        int fd_;
        fd_guard(const fd_guard& fg)=delete;
        fd_guard& operator=(const fd_guard&fg)=delete;
    public:
        explicit fd_guard(int fd = -1)noexcept;
        ~fd_guard()noexcept;
        int get()const noexcept;
        int release()noexcept;
    };

    struct file_header{
        uint32_t magic_number;
        uint32_t version;
        uint64_t page_size;
        uint64_t num_pages;
        constexpr file_header(uint32_t mn = -1,uint32_t ver = 0,uint64_t ps = db::page_size,uint64_t np = 1):magic_number(mn),version(ver),page_size(ps),num_pages(np){}
        void write_header(page & p)const;
        static file_header read_header(const page& p);
    };

    class file_handle
    {
    private:
        using page_id = db::page_id;
        using file_id = db::file_id;
        using page = db::page;

        page_id num_pages;
        file_id id;
        int os_id;
        bool dirty;
        off_t calc_offset(page_id idx,bool append_back = false)const;
        file_handle(const file_handle & fh) = delete;
        file_handle & operator=(const file_handle & fh) =delete;
    public:
        file_handle();
        file_handle(page_id npg,file_id fid,int oid);
        ~file_handle();
        bool is_open()const;
        void read_page(page_id idx,page& dest)const;
        void write_page(page_id idx,const page & src);
        page_id allocate_page();
        void close();
        void append_page(const page & src);
        file_id get_id()const noexcept;
    };


    class file_manager
    {
    private:
        file_id next;
        std::unordered_map<file_id,file_handle*> file_handles;
        std::unordered_map<std::string,file_id> filenames;
        std::unordered_map<file_id,std::string> idx2name;
        file_manager(const file_manager & fm) = delete;
        file_manager & operator=(const file_manager & fm) =delete;
    public:
        file_manager();
        ~file_manager();
        void create_file(const std::string & path)const;
        file_id open_file(const std::string & path);
        void close_file(file_id id);
        void remove_file(const std::string & path);
        file_handle & get_file(file_id id);
    };
}