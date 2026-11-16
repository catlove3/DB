# IDBS：数据库系统实验工程

面向第一次实现数据库引擎的学生开发者，根据清华大学 [数据库系统概论实验教程](https://thu-db.github.io/dbs-tutorial/) 整理开发手册，并提供 C++17 工程骨架。

当前已实现页式文件管理、单线程缓存池、page_guard、定长记录页、记录文件的初始化与增删改查、记录扫描和行编码。catalog 模块支持 JSON 元数据保存、数据库创建/选择/删除、普通建表、表的查询/打开/关闭/删除及 table_lease 借用管理，并有对应测试。主键、外键的定义转换已实现；索引创建、约束执行、SQL 解析和执行尚未实现，带主键或外键的建表请求暂时报 `feature_unsupported`。CLI 尚未接入存储和 catalog，输入 SQL 会返回明确的 `!ERROR`。仓库根目录 `IDBS/` 同时是构建、课程评测和示例命令的工作目录。

## 编译与运行

需要 CMake 3.21+ 和支持 C++17 的编译器。CLI 测试另需 Python 3；当前构建不需要 Java 或 ANTLR。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure --no-tests=error
./build/mydb --help
./build/mydb
```

CTest 包含 catalog 单元测试和 `catalog_restart` 集成测试。后者启动三个独立进程，验证跨页记录的插入、更新、删除，以及数据库删除和重建在正常退出后能够持久保存；Linux 下还通过故障注入验证写入、关闭、重命名和文件删除失败的处理。

交互输入 `exit` 或读到 EOF 会正常退出。批处理检查可运行：

```bash
printf 'exit\n' | ./build/mydb -b
printf 'SHOW DATABASES;\n' | ./build/mydb -b
```

第二条命令当前返回“SQL execution is not implemented”的错误结果块，说明输入输出入口已经连通。`--init` 目前只直接以 0 退出；`--data_dir` 仅保存路径参数。接入目录和引擎后再补充数据初始化与清理。`-d` 和 `-f`/`-t` 会明确报告功能尚未实现。

当前使用 VS Code + WSL，在 WSL 终端中执行以下命令打开工程：

```bash
cd /home/zypca/IDBS
code .
```

Windows 端安装微软的 WSL 扩展；打开后左下角应显示 `WSL: ...`。在这个窗口中安装推荐的 C/C++ 和 CMake Tools 扩展，确保它们安装在 WSL 中。按 `Ctrl+Shift+B` 配置并编译 Debug；通过“终端 → 运行任务”选择 `IDBS: 运行` 或 `IDBS: 测试`。

## 开发资料

开发任务书、接口契约、实现指南和审查记录保存在本地 `docs/`，由 `.gitignore` 排除，不随仓库提交；新克隆的仓库不包含这些文档。函数接口说明保留在源码注释中。

[示例与预期结果](examples/README.md) 随仓库维护，包含基础功能、约束拒绝、重启验证和批量导入。`testcase.yml` 已配置必做测例类别，但当前骨架还不能通过这些数据库功能测例。
