# 底层错误和文件生命周期

更新日期：2026-10-31。本文说明已实现的异常接口及本轮底层修正；记录、目录、索引和 SQL 引擎仍待实现。

## 统一异常

公共类型位于 [error.h](../include/db/common/error.h)。CLI 参数校验、文件管理和缓存池主动报告的错误统一使用 `db_error`，它继承 `std::runtime_error`，所以原先捕获 `std::exception` 的边界仍然有效。内存分配失败等标准库异常仍原样传播，不能把 bad_alloc 当作可恢复业务错误。

```cpp
try {
    auto guard = bpm.fetch_page(fid, pid);
    // 在 guard 存活期间读取 guard.view()。
} catch (const db::db_error& error) {
    const auto code = error.code();
    const auto& context = error.context();
    // 程序根据 code 分支，what() 只用于诊断。
    // error_code_name(code) 提供稳定的错误码名称。
    (void)code;
    (void)context;
}
```

context 拥有自己的数据：operation，以及可选的 path、object_id、page、slot、line、column、os_error、cleanup_os_error。os_error 在失败的系统调用处保存；清理失败不覆盖原始错误，另外存入 cleanup_os_error。不存在的上下文保持 nullopt，不能用 0 代替，因为文件 ID 和系统错误值的零值有独立含义。

| 情形 | 错误码 |
| --- | --- |
| 非法 CLI 参数、缓存容量或访问方式 | invalid_argument |
| 创建路径已存在 | object_exists |
| 打开/删除的路径不存在，或创建的父目录不存在 | object_not_found |
| 同一文件或其硬链接已打开；仍有 pinned 页 | resource_busy |
| file_id 不存在，或使用已关闭句柄 | invalid_file_handle |
| 越界页、页偏移无法完整容纳一页、通过缓存访问 page 0 | invalid_page |
| 错误文件头、短文件或意外 EOF | invalid_format |
| 系统读写/关闭失败 | io_error，带 errno |
| 没有可淘汰缓存帧 | buffer_exhausted |
| 缓存内部映射或链结构不一致 | internal_error |

异常类别不等于可以安全重试。参数拒绝一般未修改数据；I/O 失败可能已经发生部分写入。调用方不能捕获所有 db_error 后一律继续修改文件。

## 文件关闭的两种失败状态

`file_handle::close()` 先保存文件头页数，再执行系统 close。

1. 文件头写失败时，fd 仍由句柄持有；显式调用者能够报告失败并决定是否重试。manager 不移除这一句柄。
2. 真正调用系统 close 前，句柄先使自己的 fd 失效。即使系统 close 返回错误，也不再对原号码重试；manager 移除其打开登记，并将错误继续上抛。

这个区分适用于当前 Linux/WSL 平台。Linux 可能在报告关闭错误之前已经释放了 fd，重试可能误关随后复用该号码的其他文件。[Linux close 文档](https://man7.org/linux/man-pages/man2/close.2.html)

析构保留不抛异常的兜底路径：即使保存文件头失败，仍释放最终持有的 fd。析构不能报告持久化成功，因此正常退出必须显式关闭并检查错误。

## 创建与物理文件身份

创建使用排他创建，目标原本存在时不覆盖。若本次创建后的文件头写入或关闭失败，清理本次创建的文件；清理也失败时，在异常中同时保留原始原因、残留路径和 cleanup_os_error。

同一个 file_manager 使用设备号与 inode 识别已经打开的物理文件。普通路径、符号链接和硬链接不能绕过重复打开及已打开文件删除检查。管理器使用独占所有权管理句柄；调用者通过 get_file 取得的是借用引用，不得删除。

这些检查作用于同一个管理器，不提供跨进程锁或与外部进程并发重命名的完整事务隔离。未来 engine 应统一管理同一数据目录的所有句柄和锁。

## 页零与缓存关闭入口

page 0 是文件管理器独占的文件头：BPM 的 fetch/flush/evict/discard 拒绝该页，file_handle 的普通 write_page 也拒绝该页。底层 read_page(0) 可用于诊断；文件头更新通过内部专用写入路径完成。

fetch_page 在查看缓存和选择牺牲页之前检查句柄、页号和 access。非法请求不会因为缓存恰好命中而读到已关闭文件的旧数据，也不会先淘汰健康的脏页。被入口校验拒绝的请求不增加 hit/miss；有效缺页请求后续因 I/O 或缓存耗尽失败仍计入 miss。

上层关闭文件统一使用：

```cpp
// 首先结束该文件的所有游标，释放所有 page_guard。
bpm.close_file(fid);
```

此方法先验证 fid，再刷新并失效缓存，最后调用 fm.close_file。仍有 pin 时返回 resource_busy，不关闭文件；刷写失败时保留缓存映射；文件头保存失败时文件仍打开，但已成功刷新的缓存可能已被移除。这个过程不是事务回滚。

fm.close_file 仍可用于完全未经过 BPM 的独立文件。已使用 BPM 的文件不得绕过上述入口直接关闭；低层类不反向持有 BPM。BPM shutdown 仍只刷新并清空缓存，不关闭文件。

## 保留的能力边界

- 页追加成功后发生缓存映射分配失败，仍可能遗留磁盘零页；目前没有撤销分配或 WAL。
- pwrite 部分成功后报错，不保证原页字节完整保留。
- 正常关闭后可以重开读取，不提供强杀或断电恢复承诺。
- guard 与其借用指针必须先于 BPM 失效，BPM 必须先于 file_manager 销毁。

对应测试覆盖 typed error、硬链接、页零拒绝、关闭顺序，以及通过系统调用包装模拟的短读写、EINTR、创建失败和关闭失败。运行方式见 [缓存池文档](BUFFER_POOL.zh-CN.md)。

2026-10-31 在 WSL 实际运行 `make test` 和 `make sanitize`，四个 CTest 入口均通过：文件管理、存储故障注入、缓存池、CLI。故障注入包含 9 组场景；缓存池包含 10 组及 500 次固定种子的随机操作。sanitize 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。
