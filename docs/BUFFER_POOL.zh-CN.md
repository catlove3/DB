# Buffer Pool Manager 使用与实现

实现位于 [`buffer_pool.h`](../include/db/storage/buffer_pool.h) 和
[`buffer_pool.cpp`](../src/storage/buffer_pool.cpp)，测试位于
[`test_buffer_pool.cpp`](../tests/unit/test_buffer_pool.cpp)。本模块是单线程页缓存，
通过 `file_manager` 进行同步页读写；页大小为 8192 字节。

## 1. 页、帧和索引

`page_id` 是文件内的页号，`frame_id` 是缓冲池内存槽的编号。页的完整身份是
`(file_id, page_id)`，两个文件可以拥有相同页号。文件的第 0 页由文件管理器保存
文件头，`new_page()` 分配的数据页从第 1 页开始。

每个 `frame` 保存页字节、文件 ID、页号、`valid`、`dirty` 和 `pin_count`。
构造时固定分配所有 frame，guard 存活期间底层页地址不会因扩容而变化。

| 结构 | 内容与用途 |
| --- | --- |
| `frames` | 固定容量的内存槽，每槽容纳一页 |
| `table` | `(file_id, page_id) → frame_id`，定位驻留页 |
| `file_pages` | `file_id → unordered_set<page_id>`，列出该文件的驻留页 |
| `free_frames` | 所有无效、可直接使用的 frame |
| `lru` | 所有有效 frame 的访问顺序，并单独记录 pin 标记 |

`file_pages` 只包含当前驻留页，不表示文件的全部已分配页。最后一个驻留页移除时，
对应文件桶也会删除。`flush_file()` 和 `flush_and_invalidate_file()` 只访问该文件
的集合，再通过 `table` 定位 frame，不扫描整个缓冲池。

设缓冲池有 N 帧、某文件驻留 K 页，哈希操作的平均复杂度如下：

| 操作 | 平均时间复杂度 |
| --- | --- |
| 命中 fetch、pin/unpin、已知页的移除 | O(1) |
| 查找可淘汰帧 | 最坏 O(N)，从 LRU 尾部跳过 pinned 帧 |
| 按文件刷新、按文件刷新并失效 | O(K)，另加磁盘 I/O |
| flush_all、shutdown、check_invariants | O(N)，另加磁盘 I/O |

## 2. 获取页与新建页

```cpp
#include "db/storage/buffer_pool.h"

db::file_manager fm;
fm.create_file("example.db"); // 新文件示例，路径应尚不存在
db::file_id fid = fm.open_file("example.db");

{
    db::buffer_pool_manager bpm(fm, 3);
    db::page_id pid;
    {
        auto writer = bpm.new_page(fid);
        pid = writer.page_no();
        writer.get()->fill(std::byte{0x2a});
    } // writer 析构，释放一个 pin

    {
        auto reader = bpm.fetch_page(fid, pid);
        const db::page * bytes = reader.view();
        // 使用 bytes；reader 释放后不再使用这个指针。
        (void)bytes;
    }

    bpm.check_invariants();
    bpm.shutdown();
}
fm.close_file(fid);
```

`fetch_page()` 命中时更新 LRU 顺序并返回 guard。缺页时优先从 free list 取帧；
没有空帧时，从 LRU 尾部查找未被 pin 的候选。脏候选成功写回后才删除旧映射，
随后读取新页并发布两份索引。全部帧被 pin 时抛出 `std::runtime_error`。

`new_page()` 先取得帧，再调用 `file_handle::allocate_page()`，使缓存耗尽时不会
额外分配磁盘页。磁盘分配会写入零页，因此缓存中的新页初始为干净的零页。
请求可写字节后才标脏。它返回可写 guard。

帧数必须大于 0，且不能超过 `invalid_frame_id`；非法容量抛出
`std::invalid_argument`。BPM 不可复制或移动。

## 3. Guard、pin 和 dirty

`page_guard` 只能移动。每次构造增加一个 pin，每次 `release()` 或析构释放一个
pin；移动只转移已有 pin，移动赋值会先释放目标原先持有的页。重复 release 安全。
一个页可以同时被多个 guard 持有，最后一个 guard 释放后才可淘汰。

| Guard 方法 | 行为 |
| --- | --- |
| `view()` | 返回只读页指针；失效时返回 nullptr |
| `get()` | 可写 guard 返回可写页指针并标脏；只读或失效时返回 nullptr |
| `frame()` | 返回帧编号；失效时为 `invalid_frame_id` |
| `file()` / `page_no()` | 有效时返回页身份；失效时返回 0 |
| `writable()` / `operator bool()` | 查询写权限和有效性 |

文件 ID 和页号本身允许为 0，所以应使用 `operator bool()` 判断 guard 是否有效。
从 guard 获取的指针不能超出 guard 的生命周期。所有 guard 必须先于 BPM 销毁，
BPM 必须先于它引用的 file_manager 销毁。

刷新可以在 guard 存活时执行。成功写回后，如果还有 pin，保守地保留 dirty，
以覆盖调用者通过之前获得的可写指针继续修改的情形；没有 pin 时才清除 dirty。
因此，持有 guard 时反复刷新同一脏页可能重复写入。释放 guard 本身不执行磁盘 I/O。

## 4. 刷新、淘汰和关闭

| 接口 | 成功行为 / 返回值 | 未缓存或仍有 pin 时 |
| --- | --- | --- |
| `flush_page(fid, pid)` | 保留缓存；已缓存返回 true，即使本来干净 | 未缓存返回 false；允许 pin |
| `flush_file(fid)` | 刷新该文件脏页，返回成功写回页数 | 无驻留页返回 0；允许 pin |
| `flush_all()` | 刷新全部脏页，返回成功写回页数 | 允许 pin |
| `evict_page(fid, pid)` | 必要时写回、删除索引并归还 frame | 未缓存或 pinned 返回 false |
| `discard_page(fid, pid)` | 放弃缓存内容、删除索引并归还 frame | 未缓存或 pinned 返回 false |
| `flush_and_invalidate_file(fid)` | 刷新并移除该文件所有驻留页，返回移除页数 | 无驻留页返回 0；存在 pin 则抛异常 |
| `shutdown()` | 刷新并清空缓存，保留统计信息 | 存在 pin 则抛异常 |

`discard_page()` 不写回脏内容，也不释放磁盘页。只有确定要放弃内存修改或删除
文件时才使用它；后续 fetch 仍会读取磁盘上的旧内容。

关闭一个文件的顺序是：释放该文件的所有 guard，再执行：

```cpp
bpm.flush_and_invalidate_file(fid);
fm.close_file(fid);
```

该操作先检查整个文件的 pin，再刷新所有脏页，全部写入成功后才逐页移除映射。
关闭失败时可以保留缓存并处理异常。文件重开会获得新的 file ID。

`shutdown()` 先检查所有 pin，再刷新，然后清空页表、文件索引和 LRU、重置 frame、
恢复 free list。重复调用安全；当前实现允许 reset 后继续使用同一个 BPM。
它不关闭文件句柄。正常退出应显式调用它，让写回错误传播给上层。

析构函数逐帧尝试写回并吞掉异常，某帧失败不会阻止其余帧写回。析构只能兜底，
无法向调用者报告持久化失败。刷新通过文件管理器执行完整页 pwrite，未调用 fsync，
保证正常关闭重开后的内容一致，不提供断电恢复保证。

## 5. 异常时保留什么

- 读取失败：目标 frame 重置并归还 free list，不发布目标页映射。已经成功淘汰
  的旧页不会重新装回。
- 两份索引的分配失败：`add_mapping()` 撤销已完成的部分插入，再由调用者归还 frame。
- 脏页写回失败：保留 dirty、旧页身份、两份索引及 LRU 状态，不把 frame 交给新页。
- 按文件失效或 shutdown 的写回失败：不执行移除阶段。此前已成功写回的页可以变干净，
  未成功的页继续保持 dirty；操作不是磁盘事务。
- 新建磁盘页成功、索引分配失败：回收内存 frame，已分配的磁盘零页保留。
  当前文件接口没有撤销分配能力。

文件仍有驻留页时，不应绕过 BPM 关闭文件或直接修改同一个磁盘页，否则可能读到
旧缓存或使后续写回失败。文件头的页数由 file_manager 在关闭文件时维护，应由文件
管理器管理这部分元数据。

## 6. 统计与不变量检查

| 计数 | 含义 |
| --- | --- |
| `hits` | fetch 找到驻留页的次数 |
| `misses` | fetch 未找到驻留页的次数，包括后续读取失败或缓存耗尽的请求 |
| `evictions` | 容量不足触发的成功淘汰，以及显式 evict 的成功次数 |
| `writebacks` | 成功完成的脏页写回次数；同一页重复写回分别计数 |

new_page 不计 hit/miss；discard、文件失效和 shutdown 清理不计 eviction。
`stats()` 返回只读引用，shutdown 不清零计数。

`check_invariants()` 是显式调用的调试方法，发现问题抛出 `std::logic_error`。
它使用 O(N) 时间和 O(N) 临时空间，检查：

- 有效 frame、页表和文件索引的身份与数量双向一致，没有空文件桶。
- 有效 frame 在 LRU 中且不在 free list 中；无效 frame 的关系相反。
- 无效 frame 无 dirty、无 pin；LRU pin 标记与 pin_count 一致。
- 空闲链表和 LRU 链表的可达节点、计数、头尾和前后链接一致，且没有环。

在公开操作完成、回到稳定状态后调用。它不会自动插入每次 fetch 的热路径。

## 7. 构建与测试

从仓库根目录运行，Makefile 会调用 CMake，无须先手动配置：

```bash
make                         # Debug 编译
make test                    # Debug 全部测试
make test-buffer-pool        # 只运行 BPM 测试
make release                 # Release 编译与全部测试
make sanitize                # AddressSanitizer + UndefinedBehaviorSanitizer
make test JOBS=4             # 调整编译并发
```

默认目录分别为 build-debug、build-release、build-sanitize，可通过 BUILD_DIR、
RELEASE_DIR、SANITIZE_DIR 覆盖。Sanitizer 需要 GCC 或 Clang；也可手动配置
`-DIDBS_ENABLE_SANITIZERS=ON`。Makefile 之外，原有 CMake 命令继续可用。

BPM 测试使用独立临时目录，覆盖辅助结构、guard 移动、pin、LRU、3 帧写 10 页、
关闭重开、文件索引隔离、flush 后继续写、discard、实际 I/O 失败、析构兜底，
以及固定随机种子的 500 次多文件操作。测试检查实际磁盘字节，并在状态变化后
调用 check_invariants；Release 构建也会执行检查。
