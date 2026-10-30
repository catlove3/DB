#pragma once

#include "db/common/types.h"
#include "db/common/error.h"
#include <cstddef>
#include <map>
#include <memory>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <utility>
#include <sys/types.h>   // off_t

namespace db{
    /** @return src 起始的 4 字节小端无符号数。@pre src 指向至少 4 个可读字节；不检查空指针/长度。 */
    uint32_t read_u32(const void * src)noexcept;
    /** @return src 起始的 8 字节小端无符号数。@pre src 指向至少 8 个可读字节；不检查空指针/长度。 */
    uint64_t read_u64(const void * src)noexcept;
    /** @brief 将 num 编码为 4 字节小端数。@pre dest 指向至少 4 个可写字节；仅修改这 4 字节。 */
    void write_u32(void * dest,uint32_t num)noexcept;
    /** @brief 将 num 编码为 8 字节小端数。@pre dest 指向至少 8 个可写字节；仅修改这 8 字节。 */
    void write_u64(void * dest,uint64_t num)noexcept;
    /**
     * @brief 从绝对偏移读取恰好 n 字节，循环处理短读和 EINTR，不改变 fd 的共享文件偏移。
     * @param os_id 借用的可读描述符；不关闭它，n == 0 时不调用系统 I/O。
     * @param dest 指向至少 n 个可写字节；n == 0 时允许 nullptr。
     * @param n 字节数；调用者负责内存实际长度，函数只能检查空指针和数值范围。
     * @param offset 非负偏移，offset + n 必须可由 off_t 表示。
     * @throws db_error invalid_argument：空缓冲区/非法范围；invalid_format：提前 EOF；
     *         io_error：非 EINTR 的系统读错误，context.os_error 保存当时 errno。
     * @note 验证失败不读数据；I/O 途中失败可能已修改 dest 的前缀，不清零也不回滚。
     *       底层上下文只有操作名及适用的 errno，不自动带文件路径/逻辑文件号。
     */
    void safe_read(int os_id,void * dest,size_t n,off_t offset);
    /**
     * @brief 向绝对偏移写入恰好 n 字节，循环处理短写和 EINTR，不改变共享文件偏移。
     * @param os_id 借用的可写描述符；不关闭它，n == 0 时不调用系统 I/O。
     * @param srcc 指向至少 n 个可读字节；n == 0 时允许 nullptr，源内容不修改。
     * @param n 待写字节数；不检查源缓冲区实际容量。
     * @param offset 非负偏移，offset + n 必须可由 off_t 表示。
     * @throws db_error invalid_argument：空缓冲区/非法范围；io_error：非 EINTR
     *         的系统写错误或零进展写；零进展错误没有对应 os_error。
     * @note 验证失败不写盘；途中失败可能已写入前缀，不提供回滚、原子写或 fsync 保证。
     */
    void safe_write(int os_id,const void*srcc,size_t n,off_t offset);
    /**
     * @brief 返回 absolute + weakly_canonical 处理后的路径字符串。
     * @param path 非空路径；现存前缀中的符号链接会被解析，尾部允许尚不存在。
     * @return 拥有自身存储的规范化绝对路径；不创建路径，也不保证目标存在。
     * @throws db_error invalid_argument：空路径；io_error：路径解析失败。
     * @note 不是路径锁或访问权限检查，不能排除外部改名竞态；分配等标准异常可传播。
     */
    std::string normalize_path(const std::string&path);

    /** @brief 单一所有者的临时描述符守卫；不允许复制，也没有移动接口。 */
    class fd_guard
    {
    private:
        int fd_;
        /** @brief 禁止复制，避免对同一描述符重复 close。 */
        fd_guard(const fd_guard& fg)=delete;
        /** @brief 禁止复制赋值，描述符所有权通过 release() 显式移交。 */
        fd_guard& operator=(const fd_guard&fg)=delete;
    public:
        /** @brief 接管 fd；-1 表示空守卫。@pre 非 -1 时调用者不再自行关闭该描述符。 */
        explicit fd_guard(int fd = -1)noexcept;
        /** @brief 非空时尝试 close 一次；忽略错误，不重试、不抛异常。 */
        ~fd_guard()noexcept;
        /** @return 借用的描述符或 -1；所有权仍属于此守卫，调用者不得关闭它。 */
        int get()const noexcept;
        /** @return 原描述符并转移关闭责任；守卫变为空，重复调用返回 -1，不执行 close。 */
        int release()noexcept;
    };

    struct file_header{
        uint32_t magic_number;
        uint32_t version;
        uint64_t page_size;
        uint64_t num_pages;
        /**
         * @brief 直接保存四个头字段，不校验格式。
         * @note 默认 magic 为 uint32_t(-1)、version 为 0，默认构造的头不是有效磁盘格式。
         *       num_pages 包含第 0 页；有效文件使用 magic=0x114514、version=1、page_size=8192。
         */
        constexpr file_header(uint32_t mn = -1,uint32_t ver = 0,uint64_t ps = db::page_size,uint64_t np = 1):magic_number(mn),version(ver),page_size(ps),num_pages(np){}
        /**
         * @brief 将四个字段按小端写入 p 的前 24 字节（偏移 0/4/8/16）。
         * @param p 调用者拥有的整页缓冲区；其余字节保持原值，不做 I/O 或格式校验。
         */
        void write_header(page & p)const;
        /**
         * @brief 解码并校验页头，输入页不变。
         * @param p 至少包含完整头字段的 page；函数不核对实际文件长度或尾随字节。
         * @return 按值返回有效头，num_pages 在 [1, floor(off_t::max / 8192)]。
         * @throws db_error(invalid_format) magic、版本、页大小或页数非法；直接调用时上下文为空。
         */
        static file_header read_header(const page& p);
    };

    /**
     * @brief 拥有一个系统描述符及内存页数的文件句柄，非线程安全。
     * @note 经 file_manager 获得时只借用此对象，生命周期由管理器决定。
     *       存在缓存页时须通过 buffer_pool_manager 协调写入/关闭，避免绕过缓存造成旧读。
     *       成功写入只表示系统接受数据；没有事务、fsync 或崩溃恢复承诺。
     */
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
        std::string path_;
        /** @brief 生成拥有 operation/fid/可选 pid 和非空 path_ 的上下文；可能分配，不改句柄。 */
        error_context context(const std::string& operation,
                              std::optional<page_id> pid = std::nullopt) const;
        /**
         * @brief 验证句柄打开、完整页偏移可表示；非追加模式还要求 idx < num_pages。
         * @pre 追加调用者传入当前 num_pages；append_back 只跳过逻辑页数边界校验。
         * @return idx * 8192。失败为 invalid_file_handle/invalid_page，句柄和文件不变。
         */
        off_t calc_offset(page_id idx,bool append_back = false)const;
        /** @brief 禁止复制描述符所有权。 */
        file_handle(const file_handle & fh) = delete;
        /** @brief 禁止复制赋值描述符所有权。 */
        file_handle & operator=(const file_handle & fh) =delete;
    public:
        /** @brief 构造关闭的空句柄，文件号和页数均为 0；不打开文件。 */
        file_handle();
        /**
         * @brief 接管 oid 并保存元数据，不读取、校验文件或登记到 file_manager。
         * @param npg 含第 0 页的逻辑页数；调用者须提供与文件一致的有效值。
         * @param fid 调用者分配的逻辑文件号；函数不保证全局唯一。
         * @param oid 由此对象独占关闭的描述符，-1 表示关闭；不得由外部重复关闭。
         * @param path 诊断用路径，按值保存；允许为空，不在此处规范化。
         */
        file_handle(page_id npg,file_id fid,int oid, std::string path = {});
        /**
         * @brief 尝试保存页数并关闭；错误不外抛。页头保存失败后仍尝试一次最终描述符关闭。
         * @note 已提交过的系统 close 不再重试。需获知写入失败时必须先显式 close()。
         */
        ~file_handle()noexcept;
        /** @return 本对象是否仍持有非 -1 描述符；不探测外部关闭或文件系统状态。 */
        bool is_open()const;
        /**
         * @brief 将现有页完整读入 dest；允许诊断读取第 0 页。
         * @param idx 范围 [0, get_num_pages())；整个页偏移必须可表示。
         * @param dest 调用者拥有的页；成功后包含完整页，途中失败可能只更新了前缀。
         * @throws db_error invalid_file_handle、invalid_page、invalid_format（短文件）或 io_error。
         * @note 不访问缓冲池；命中缓存中的最新修改不会自动反映在这里。
         */
        void read_page(page_id idx,page& dest)const;
        /**
         * @brief 将 src 写入现有数据页；第 0 页由文件管理器独占，禁止直接覆盖。
         * @param idx 范围 [1, get_num_pages())。
         * @param src 借用整页，只读，调用结束即可释放。
         * @throws db_error invalid_file_handle、invalid_page 或 io_error。
         * @note 参数失败不写盘；I/O 失败可能部分写盘。页数不变，不使缓冲池已有副本失效。
         */
        void write_page(page_id idx,const page & src);
        /**
         * @brief 在逻辑文件尾追加一页全零数据。
         * @return 新页号，即调用前 get_num_pages()；成功后页数加 1，页头标记待保存。
         * @throws db_error invalid_file_handle、invalid_page（地址空间耗尽）或 io_error。
         * @note 失败不增加内存页数，但磁盘尾部可能已有部分写入；不截断回滚。
         */
        page_id allocate_page();
        /**
         * @brief 先保存变化的页数，再关闭描述符；已关闭时幂等返回。
         * @throws db_error(io_error) 页头写入或系统 close 失败。
         * @note 页头写失败：保持打开和 dirty，可显式再次处理，磁盘头可能已部分改写。
         *       已调用系统 close：无论返回值如何，本对象都变为关闭，不重试原描述符。
         *       不处理缓冲池脏页；管理器所拥有的句柄应由管理器协调关闭。
         */
        void close();
        /**
         * @brief 将 src 写到当前逻辑文件尾；仅完整写成功后页数加 1 并标记页头待保存。
         * @param src 借用整页，内容不修改；追加位置不采用物理文件末尾，而采用内存页数。
         * @throws db_error invalid_file_handle、invalid_page 或 io_error。
         * @note 失败保留原内存页数，磁盘可能有部分尾页；成功后页数在 close() 才写入页头。
         */
        void append_page(const page & src);
        /** @return 构造时的逻辑文件号；关闭后仍保留，不能据此断言已登记或仍打开。 */
        file_id get_id()const noexcept;
        /** @return 当前内存逻辑页数（含第 0 页）；关闭后仍保留，不执行文件长度检查。 */
        page_id get_num_pages()const noexcept{return num_pages;}
    };


    /**
     * @brief 单线程文件注册表，拥有所有登记的 file_handle；不提供跨进程锁。
     * @note 相同规范路径或相同 (device,inode) 在此实例中只能打开一次。
     *       管理器必须比借用它的缓冲池存活更久；外部不得改名、截断或替换正在管理的文件。
     */
    class file_manager
    {
    private:
        using physical_identity = std::pair<dev_t, ino_t>;
        struct open_file_entry {
            std::unique_ptr<file_handle> handle;
            std::string path;
            physical_identity identity;
        };
        file_id next;
        std::unordered_map<file_id,open_file_entry> file_handles;
        std::unordered_map<std::string,file_id> filenames;
        std::map<physical_identity,file_id> identities;
        /** @brief 同步移除三个注册索引并销毁句柄；不存在时无操作。@pre 正常关闭路径已处理描述符。 */
        void unregister_file(file_id id)noexcept;
        /** @brief 禁止复制注册表及其描述符所有权。 */
        file_manager(const file_manager & fm) = delete;
        /** @brief 禁止复制赋值注册表及其描述符所有权。 */
        file_manager & operator=(const file_manager & fm) =delete;
    public:
        /** @brief 构造空注册表，下一文件号为 0；不创建磁盘目录或文件。 */
        file_manager();
        /** @brief 销毁所有句柄，由其析构尽力保存/关闭；不报告写失败，所有借用引用失效。 */
        ~file_manager();
        /**
         * @brief 排他创建带有效第 0 页的新文件，并关闭临时描述符；不会登记或返回文件号。
         * @param path 非空路径，先规范化；父目录须存在，不覆盖现有目标。
         * @throws db_error invalid_argument、object_exists、object_not_found 或 io_error。
         * @note 成功文件只有一页（8192 字节），头版本 1，其余初始字节为 0。
         *       创建后的写入/关闭失败会尝试删除本次新文件；删除失败可能留文件，
         *       对 db_error 在 cleanup_os_error 中记录 unlink 错误，保留首个错误。
         *       分配等标准异常原样传播；清理本身的 close 错误不会另行报告。
         */
        void create_file(const std::string & path)const;
        /**
         * @brief 以读写方式打开普通文件，验证头和最小长度，再原子登记内部索引。
         * @param path 待打开路径，先规范化；相同路径或硬链接别名已登记时拒绝。
         * @return 此实例内递增且不复用的文件号；首号为 0，最大 uint64_t 值不分配。
         * @throws db_error invalid_argument、object_not_found、resource_busy、resource_limit、
         *         invalid_format（非普通文件/坏头/短文件）或 io_error；分配失败可抛标准异常。
         * @note 失败不留下新登记，临时描述符由守卫清理；不修复文件，不要求实际长度
         *       恰等于头声明长度，允许尾随数据。仅保证本管理器内避免重复打开。
         */
        file_id open_file(const std::string & path);
        /**
         * @brief 关闭已登记文件并注销其路径/物理身份/句柄索引。
         * @param id 已登记的逻辑文件号；若文件参与缓冲池，改用其 close_file() 协调生命周期。
         * @throws db_error invalid_file_handle（未登记，包括重复调用）或 io_error。
         * @note 页头写失败保持登记、句柄和借用引用有效；一旦系统 close 已提交，无论
         *       成败都注销并销毁句柄，既有 get_file() 引用失效。不会主动刷缓冲池。
         */
        void close_file(file_id id);
        /**
         * @brief 删除规范化路径的目录项；不允许删除本实例已打开文件的路径或硬链接别名。
         * @param path 非空目标路径；符号链接按 normalize_path 解析，可能指向链接目标。
         * @throws db_error invalid_argument、object_not_found、resource_busy 或 io_error。
         * @note 不自动关闭文件，不递归删除目录；成功仅表示 unlink 完成，无 fsync 保证。
         *       无外部并发修改前提下，前置检查失败不删除目标；不提供跨进程竞态防护。
         */
        void remove_file(const std::string & path);
        /**
         * @brief 查找注册表中的句柄，不打开文件，也不检查其系统描述符是否被直接关闭。
         * @param id 此管理器登记的文件号。
         * @return 管理器拥有的借用引用；注销/管理器销毁后不可再访问，不得自行 delete。
         * @throws db_error(invalid_file_handle) 文件号未登记；注册表不变。
         */
        file_handle & get_file(file_id id);
    };
}
