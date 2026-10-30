#pragma once
#include <db/common/types.h>
#include <db/common/error.h>
#include <db/storage/file_manager.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace db
{

constexpr frame_id invalid_frame_id = std::numeric_limits<frame_id>::max();

class buffer_pool_manager;

enum class access_type
{
    read,
    write
};

struct buffer_pool_stats
{
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t writebacks = 0;
};

struct page_key
{
    file_id fid = 0;
    page_id pid = 0;
    /** @return 两个键的文件号和页号是否均相等；不查询文件注册表。 */
    bool operator==(const page_key & pk)const noexcept;
};

struct hash_page_key
{
    /** @return 由文件号和页号混合得到的哈希；允许碰撞，不作为持久化标识。 */
    std::size_t operator()(const page_key & k)const noexcept;
};

struct frame
{
    page _page{};
    file_id fid = 0;
    page_id pid = 0;
    std::uint32_t pin_count = 0;
    bool valid = false;
    bool dirty = false;
    /** @brief 将身份、pin 数及标志复位；不清零 _page 的 8192 字节，不执行 I/O。 */
    void reset()noexcept;
    /** @brief 直接赋值帧元数据；不改页字节、不验证参数，也不维护池的页表/LRU/空闲表。 */
    void set(file_id f,page_id p,std::uint32_t pc = 0,bool v = true,bool d = false)noexcept;
};

class page_table
{
private:
    using map_type = std::unordered_map<page_key,frame_id,hash_page_key>;
    map_type hash_table;
public:
    /** @brief 构造空的页键到帧号映射；不拥有页或帧。 */
    page_table();
    /** @return k 对应帧号；缺失返回 invalid_frame_id，不抛“未找到”异常。 */
    frame_id query(const page_key & k)const;
    /** @return (fid,pid) 对应帧号；等价于 query(page_key{fid,pid})。 */
    frame_id query(file_id fid,page_id pid)const;
    /** @return 是否含 k；不核实实际帧仍存在或文件仍打开。 */
    bool contains(const page_key & k)const;
    /** @return 是否含 (fid,pid)；等价于 contains(page_key{fid,pid})。 */
    bool contains(file_id fid,page_id pid)const;
    /**
     * @brief 插入 k 到 f 的映射；已有键不覆盖，返回 false，插入成功返回 true。
     * @pre 调用者保证 f 为合法帧号且关联关系有效；本容器不校验范围/重复帧号。
     * @note 容器分配失败可抛标准异常；单次插入失败保持原映射不变。
     */
    bool insert(const page_key & k,frame_id f);
    /** @brief 等价于 insert(page_key{fid,pid},f)，沿用相同返回值、前提和失败承诺。 */
    bool insert(file_id fid,page_id pid,frame_id f);
    /** @return 是否实际删除 k；缺失返回 false。仅删除映射，不释放帧、不写盘。 */
    bool erase(const page_key & k);
    /** @return 是否实际删除 (fid,pid)；等价于 erase(page_key{fid,pid})。 */
    bool erase(file_id fid,page_id pid);
    /** @brief 删除全部映射；不触碰池中的帧或磁盘页。 */
    void clear()noexcept;
    /** @return 是否没有映射；无副作用。 */
    bool empty()const noexcept;
    /** @return 当前映射条数，不是桶数或预留容量。 */
    std::size_t size()const noexcept;
    /** @brief 为至少 cap 个映射预留桶空间；不添加映射，容器分配异常原样传播。 */
    void reserve(std::size_t cap);
    friend class buffer_pool_manager;
};

class free_list
{
private:
    std::vector<frame_id> next;
    std::vector<bool> linked; // 1 for held, 0 for given out
    frame_id head;
    std::size_t count;
    friend class buffer_pool_manager;
public:
    /**
     * @brief 建立含 [0,cap) 全部帧号的空闲表，初始取出顺序为递增。
     * @throws db_error(invalid_argument) cap 为 0 或大于 invalid_frame_id；分配异常可传播。
     */
    free_list(std::size_t cap);
    /** @return 是否无可分配帧号。 */
    bool empty()const noexcept;
    /** @return 当前空闲帧数量。 */
    std::size_t size()const noexcept;
    /** @return fid 是否在空闲表中；越界返回 false。 */
    bool contains(frame_id fid)const noexcept;
    /** @return 移除并返回头部空闲帧号；空表返回 invalid_frame_id，表不变。 */
    frame_id get_empty_frame()noexcept;
    /** @return 将 fid 插回表头是否成功；越界/已经空闲返回 false 且表不变，不清理实际帧。 */
    bool push(frame_id fid)noexcept;
    /** @brief 恢复全部 [0,cap) 帧号为空闲，顺序递增；调用者须先释放实际占用状态。 */
    void clear()noexcept;
};

class LRU_list;

class _List
{
private:
    std::vector<frame_id> prev;
    std::vector<frame_id> next;
    std::vector<bool> linked;
    frame_id head;
    frame_id tail;
    std::size_t count;
    friend class LRU_list;
    friend class buffer_pool_manager;
public:
    /** @brief 构造容量为 cap 的空双链表；cap 可为 0，分配异常原样传播。@pre cap <= invalid_frame_id。 */
    explicit _List(std::size_t cap);
    /** @return 当前是否无成员。 */
    bool empty()const noexcept;
    /** @return 当前成员数量，不是容量。 */
    std::size_t size()const noexcept;
    /** @return 是否包含 fid；越界返回 false。 */
    bool contains(frame_id fid)const noexcept;
    /** @return 将 fid 加到表头是否成功；越界/已存在返回 false，链表不变。 */
    bool push_front(frame_id fid)noexcept;
    /** @return 将已有 id 移到表头是否成功；原本在表头也返回 true，不存在返回 false。 */
    bool move_front(frame_id id)noexcept;
    /** @return 是否移除了 fid；缺失/越界返回 false，链表不变。 */
    bool erase(frame_id fid)noexcept;
    /** @return 移除并返回表尾帧号；空表返回 invalid_frame_id。 */
    frame_id pop_back()noexcept;
    /** @brief 清除全部成员和链接，保留容量；不修改外部帧数据。 */
    void clear()noexcept;
};


class LRU_list
{
private:
    _List list;
    std::size_t _cap;
    std::vector<bool> pinned;
    friend class _List;
    friend class buffer_pool_manager;
public:
    /** @brief 构造空 LRU，所有 pin 标志为 false；cap 可为 0，分配异常可传播。@pre cap <= invalid_frame_id。 */
    LRU_list(std::size_t cap = 0);
    /** @return LRU 是否无成员；有成员但全部 pin 时仍返回 false。 */
    bool empty()const noexcept;
    /** @return LRU 中的成员总数，包括 pin 住的成员。 */
    std::size_t size()const noexcept;
    /** @return LRU 是否包含 fid，不考虑其 pin 状态；越界返回 false。 */
    bool contains(frame_id fid)const noexcept;
    /**
     * @brief 选择可淘汰帧；指定 fid 时只检查该帧，否则从尾向前找最久未使用的非 pin 帧。
     * @param remove true 时从 LRU 移除选中帧，false 只查询；不修改实际帧或页表。
     * @param fid 默认 invalid_frame_id 表示自动选择。
     * @return 候选帧号；缺失/被 pin/无候选返回 invalid_frame_id，LRU 不变。
     */
    frame_id victim(bool remove = true,frame_id fid = invalid_frame_id)noexcept;
    /** @return 将 fid 插入或移到最近使用端是否成功；越界返回 false，不改变 pin 标志。 */
    bool record_access(frame_id fid)noexcept;
    /** @brief 清空 LRU 并清除所有 pin 标志；不操作实际帧。 */
    void clear()noexcept;
    /** @brief 将范围内 fid 标记为不可淘汰；越界忽略，重复调用不累计 pin 次数。 */
    void pin(frame_id fid)noexcept;
    /** @brief 清除范围内 fid 的 pin 标志；越界忽略，不维护计数，计数由缓冲池负责。 */
    void unpin(frame_id fid)noexcept;
};



/**
 * @brief 只可移动的页借用凭证；存活时保持一份 pin，不拥有缓冲池或磁盘文件。
 * @note 缓冲池必须比所有 guard 及其借用页指针活得更久；持有期间禁止外部直接关闭文件。
 *       页指针不得超出这次 pin 的有效期；移动后由接收方 guard 继续承担同一 pin。
 *       此类型不提供锁、事务或磁盘持久性。
 */
class page_guard
{
private:
    frame_id f;
    buffer_pool_manager* bpm;
    bool can_write;
    /** @brief 为池内有效帧增加一次 pin；不分配、不执行 I/O。@pre bpm_ 非空且 fid 为有效驻留帧。 */
    page_guard(frame_id fid,buffer_pool_manager* bpm_,bool can_write_)noexcept;
    /** @brief 禁止复制同一 pin 的释放责任；需要另一份借用时重新 fetch_page。 */
    page_guard(const page_guard & ppp)=delete;
    /** @brief 禁止复制赋值，避免重复释放 pin。 */
    page_guard & operator=(const page_guard & ppp)=delete;
public:
    /** @brief 自动 release()；只减少 pin，不刷盘，不报告 I/O 错误。 */
    ~page_guard()noexcept;
    /** @brief 转移 ppp 的 pin 和读写权限，不增减 pin 数；源变为空且不可写。 */
    page_guard(page_guard && ppp)noexcept;
    /** @brief 先释放自身 pin 再接管 ppp；自移动保持原样，源变为空。@return *this。 */
    page_guard & operator=(page_guard && ppp)noexcept;
    /** @return 借用的只读整页指针；空 guard 返回 nullptr，不标脏，不延长 pin 生命周期。 */
    const page * view()const noexcept;
    /**
     * @return 借用的可写整页指针；空 guard 或只读 guard 返回 nullptr。
     * @note 返回非空指针前立即标脏，即使调用者最终没有修改；指针不能在释放 pin 后继续使用。
     */
    page* get()noexcept;
    /** @brief 释放一次 pin 并将自身置空；重复调用无副作用，不写盘、不清除 dirty。 */
    void release()noexcept;
    /** @return 持有的帧号；空 guard 返回 invalid_frame_id。 */
    frame_id frame()const noexcept;
    /** @return 所属文件号；空 guard 返回 0，但 0 也是合法文件号，须用 operator bool 判断有效性。 */
    file_id file()const noexcept;
    /** @return 所属数据页号；空 guard 返回 0，缓冲池不缓存真正的第 0 页。 */
    page_id page_no()const noexcept;
    /** @return 是否具有写权限；写权限本身不标脏，空 guard 为 false。 */
    bool writable()const noexcept;
    /** @return 是否持有非空池指针和非哨兵帧号；不探测文件描述符或池是否已被外部销毁。 */
    explicit operator bool()const noexcept;
    friend class buffer_pool_manager;
};

/**
 * @brief 单线程固定容量页缓存；只缓存 pid >= 1，独占管理这些页的缓存一致性。
 * @note fm 为借用，必须比本池存活更久；所有 guard 必须先于池析构释放。
 *       注册文件参与缓存后，通过此池 fetch/flush/close，禁止直接关闭句柄或外部修改文件。
 *       flush 只保证系统写入完成，没有 fsync、事务回滚或掉电恢复承诺。
 */
class buffer_pool_manager
{
private:
    friend class LRU_list;
    friend class _List;
    file_manager & fm;
    std::vector<frame> frames;
    page_table table;
    // Contains exactly the resident pages of each file; empty sets are removed.
    std::unordered_map<file_id, std::unordered_set<page_id>> file_pages;
    free_list free_frames;
    LRU_list lru;
    buffer_pool_stats stat;

    /** @brief 禁止复制池及帧身份，现有 guard 必须指向原池。 */
    buffer_pool_manager(const buffer_pool_manager & bpm)=delete;
    /** @brief 禁止复制赋值帧、索引及借用管理器关系。 */
    buffer_pool_manager & operator=(const buffer_pool_manager & bpm)=delete;
    /** @brief 禁止移动池，避免现有 guard 保存的池指针失效。 */
    buffer_pool_manager(buffer_pool_manager && bpm)=delete;
    /** @brief 禁止移动赋值池，避免既有 guard 引用改变身份。 */
    buffer_pool_manager & operator=(buffer_pool_manager && bpm)=delete;

    /** @return 范围内帧的借用指针，越界返回 nullptr；不校验 valid、不增加 pin。 */
    frame * get_frame(frame_id fid)noexcept;
    /** @return 范围内帧的只读借用指针，越界返回 nullptr；不校验 valid、不增加 pin。 */
    const frame * get_frame(frame_id fid)const noexcept;
    /** @return 页表中的帧号或 invalid_frame_id；不核实文件生命周期和页号范围。 */
    frame_id find_frame(file_id fid,page_id pid)const;
    /**
     * @brief 取空闲帧，必要时写回并淘汰一个未 pin 的 LRU 帧；全部 pin 时返回 invalid_frame_id。
     * @pre 池内索引一致；调用者取得帧后必须建立新映射，或在失败时归还空闲表。
     * @note 脏候选写失败保留原映射和 dirty；成功淘汰增加 evictions，之后调用者失败不撤销淘汰。
     */
    frame_id acquire_frame();
    /** @brief 为范围内帧增加 pin_count，0→1 时设置 LRU pin。@pre 有效帧且计数未溢出；越界忽略。 */
    void pin(frame_id fid)noexcept;
    /** @brief pin_count > 0 时减一，降至 0 时允许 LRU 淘汰；越界/零计数无操作。 */
    void unpin(frame_id fid)noexcept;
    /** @brief 将范围内帧标脏，越界无操作。@pre 正常调用只针对有效驻留帧。 */
    void mark_dirty(frame_id fid)noexcept;
    /**
     * @brief 有效脏帧才写回；成功增加 writebacks，仍有任何 pin 时保留 dirty。
     * @pre fr 属于此池且身份一致。失败传播文件层错误、保留 dirty 和索引，磁盘可能部分写入。
     */
    void write_back(frame & fr);
    /**
     * @brief 将已设置的帧同步加入页表和逐文件页集合。
     * @pre fid 在范围内、帧有效、目标键尚未登记；正常调用需保持映射一一对应。
     * @note 容器分配异常或检测到重复时回滚本次索引新增；不回滚磁盘分配，不重置帧。
     */
    void add_mapping(frame_id fid);
    /** @brief 移除指定帧的两份索引，删去空文件集合；越界忽略。@pre 帧身份与索引一致；不重置帧/LRU。 */
    void remove_mapping(frame_id fid);
public:
    /**
     * @brief 建立空缓存，所有帧空闲、统计为 0；不打开文件、不读写页面。
     * @param fm_ 借用文件管理器，必须比缓冲池存活更久。
     * @param frame_count 帧数量，范围 [1, invalid_frame_id]；每帧含一页 8192 字节。
     * @throws db_error(invalid_argument) 容量越界；内存分配/容器容量异常原样传播。
     */
    buffer_pool_manager(file_manager & fm_,std::size_t frame_count);
    /**
     * @brief 对每个脏帧尽力写回，逐帧忽略错误；不关闭文件、不抛异常。
     * @pre 所有 guard 已释放，fm 及相关文件仍有效；需要检查写回结果时先显式 shutdown()。
     */
    ~buffer_pool_manager()noexcept;

    /**
     * @brief 获取现有数据页的 guard；验证全部通过后才记录命中/未命中或淘汰帧。
     * @param fid 此池关联管理器中已登记且打开的文件号。
     * @param pid 范围 [1, 文件内存页数)，第 0 页禁止缓存。
     * @param access read 只能 view；write 允许 get，直到 get 时才标脏。
     * @return 按值转移的 guard，新增一份 pin；读写 guard 可以同时存在，不提供读写锁。
     * @throws db_error invalid_page、invalid_argument（访问枚举非法）、invalid_file_handle、
     *         buffer_exhausted、invalid_format 或 io_error；容器分配等标准异常可传播。
     * @note 参数/句柄验证失败不计统计；合法 miss 即使最终失败仍计 misses。
     *       读页/映射失败会归还目标帧，但之前成功的写回和淘汰不撤销。返回 guard 前不交付页指针。
     */
    page_guard fetch_page(file_id fid,page_id pid,access_type access = access_type::read);
    /**
     * @brief 先取得可用帧，再在文件尾分配全零页并缓存；返回具有写权限的 guard。
     * @param fid 已登记且打开的文件号。
     * @return 新页 guard；页最初为 clean，调用 get() 才标脏；不增加 hits/misses。
     * @throws db_error invalid_file_handle、buffer_exhausted、invalid_page 或 io_error；
     *         映射建立期间还可能传播标准分配异常。
     * @note 全部帧被 pin 时不分配磁盘页；失败归还候选帧，先前成功淘汰不回滚。
     *       allocate_page 成功后若映射分配失败，新增磁盘页和内存页数保留；没有磁盘回滚。
     */
    page_guard new_page(file_id fid);

    /**
     * @brief 写回缓存中的指定脏页，保留驻留位置；pin 住的页也允许写回。
     * @return 是否驻留：缺失为 false，clean 页也返回 true，不能用返回值判断是否发生写入。
     * @throws db_error invalid_page（pid == 0）或实际写回的文件层错误。
     * @note 缺失时不验证 fid/页号范围；成功写入计 writebacks，有任何 pin 时仍保留 dirty。
     *       失败保持映射和 dirty，磁盘可能部分改写；无 fsync 承诺。
     */
    bool flush_page(file_id fid,page_id pid);
    /**
     * @brief 写回该文件所有驻留脏页，保留缓存；不校验没有驻留页的文件号。
     * @param fid 缓存键中的文件号；无对应缓存集合时返回 0。
     * @return 本次成功写出的脏页数；包括写后因 pin 而仍为 dirty 的页。
     * @throws db_error 写回失败时传播文件层错误；停止后续页，先前成功写回和统计不回滚。
     * @note 遍历顺序不稳定；抛异常时不返回已成功页数，可以查看统计差值作诊断。
     */
    std::size_t flush_file(file_id fid);
    /**
     * @brief 按帧顺序写回所有有效脏页，保留缓存和 pin。
     * @return 本次成功写出的脏页数；不是所有驻留页数。
     * @throws db_error 写回失败时停止，已写页面和 writebacks 不回滚；其余索引保留。
     */
    std::size_t flush_all();
    /**
     * @brief 写回后移除指定未 pin 页，并归还帧；成功增加 evictions。
     * @return 成功移除为 true，缺失或被 pin 为 false；两种 false 不执行写回。
     * @throws db_error invalid_page（pid == 0）或实际写回错误；写失败保留驻留和 dirty。
     * @note 不删除磁盘页，不普遍校验缺失或 clean 页的文件句柄。
     */
    bool evict_page(file_id fid,page_id pid);
    /**
     * @brief 不写回就移除未 pin 页；调用者显式承担丢弃未保存修改的责任。
     * @return 成功为 true，缺失/被 pin 为 false；pid == 0 抛 db_error(invalid_page)。
     * @note 不删除磁盘页、不改变 hits/misses/evictions/writebacks；不验证文件仍打开。
     */
    bool discard_page(file_id fid,page_id pid);
    /**
     * @brief 预检该文件全部 pin，再写回全部脏页，全部写成功后才移除所有缓存映射。
     * @param fid 无缓存集合时返回 0，不校验文件是否登记。
     * @return 移除的驻留页总数，不是写回数；移除本身不增加 evictions。
     * @throws db_error resource_busy：有任何 pin，在写回前失败；或实际写回错误。
     * @note 写回中失败保留所有映射，但先前成功写出的页可已变 clean，统计和磁盘不回滚。
     */
    std::size_t flush_and_invalidate_file(file_id fid);
    /**
     * @brief 推荐关闭入口：验证登记 → 拒绝未释放 guard → 刷新并清缓存 → 关闭/注销文件。
     * @param fid 已登记文件号；重复调用不幂等，抛 invalid_file_handle。
     * @throws db_error invalid_file_handle、resource_busy 或文件层写入/关闭错误。
     * @note pin 检查失败不写盘；脏页写失败保留全部映射。缓存清空后的页头保存失败保持
     *       文件登记和打开状态；系统 close 已提交后即使失败也注销。先前写回/清缓存不回滚。
     */
    void close_file(file_id fid);
    /**
     * @brief 预检所有 guard 已释放，写回全部脏页，然后重置缓存；不关闭管理器中的文件。
     * @throws db_error resource_busy：在任何写回前发现 pin；或写回时传播文件层错误。
     * @note 成功后全部帧空闲，统计保留，池可再次使用；重复调用安全。
     *       写回中失败不执行缓存重置，已成功的写入、dirty 清除和统计不回滚。
     */
    void shutdown();

    /** @return 构造时固定的帧容量，不执行 I/O。 */
    std::size_t frame_count()const noexcept;
    /** @return 当前空闲帧数量，不包含尚可淘汰但仍驻留的帧。 */
    std::size_t free_frame_count()const noexcept;
    /** @return 页表中的驻留页数量，包括被 pin 和 clean/dirty 页。 */
    std::size_t cached_page_count()const noexcept;
    /**
     * @return 池拥有的统计结构借用引用，生命周期不超过池；后续操作会改变读到的值。
     * @note hits/misses 只计 fetch_page 的合法查询；evictions 计自动/显式 evict，
     *       writebacks 只计成功脏页写，shutdown 不清零。需要快照时由调用者复制结构。
     */
    const buffer_pool_stats & stats()const noexcept;
    /**
     * @brief 只读诊断页表、逐文件集合、帧、空闲链、LRU 及 pin 之间的一致性。
     * @throws db_error(internal_error) 检测到不变量破坏；临时检查数组可能抛 std::bad_alloc。
     * @note 不读写磁盘、不修复状态，不验证文件仍打开或页面内容正确；不在正常操作中自动执行。
     */
    void check_invariants()const;

    friend class page_guard;
};

}
