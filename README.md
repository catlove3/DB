# IDBS：数据库系统实验工程

面向第一次实现数据库引擎的学生开发者，根据清华大学 [数据库系统概论实验教程](https://thu-db.github.io/dbs-tutorial/) 整理开发手册，并提供 C++17 工程骨架。

当前已提供 CMake 构建、命令行入口、官方 SQL 文法和 CLI 集成测试。存储、SQL 解析和数据库操作尚未实现；输入 SQL 会返回明确的 `!ERROR`。仓库根目录 `IDBS/` 同时是构建、课程评测和示例命令的工作目录。

## 编译与运行

需要 CMake 3.21+ 和支持 C++17 的编译器。CLI 测试另需 Python 3；当前构建不需要 Java 或 ANTLR。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure --no-tests=error
./build/mydb --help
./build/mydb
```

交互输入 `exit` 或读到 EOF 会正常退出。批处理检查可运行：

```bash
printf 'exit\n' | ./build/mydb -b
printf 'SHOW DATABASES;\n' | ./build/mydb -b
```

第二条命令当前返回“SQL execution is not implemented”的错误结果块，说明输入输出入口已经连通。`--init` 目前只直接以 0 退出；`--data_dir` 仅保存路径参数。实现存储后再补充数据初始化与清理。`-d` 和 `-f`/`-t` 会明确报告功能尚未实现。

Windows/Visual Studio 请打开仓库根目录中的 CMake 工程，具体操作、可执行文件位置和 Debug/Release 预设见下面的工程说明。

## 开发资料

- [构建与工程结构](docs/BUILD.zh-CN.md)：目录职责、Visual Studio 使用方式、增加模块和测试的方法。
- [完整开发者文档](docs/DEVELOPER_GUIDE.zh-CN.md)：需求边界、架构、页与记录布局、B+ 树、SQL 解析、约束、执行器、持久化、测试与调试。
- [示例与预期结果](examples/README.md)：基础功能、约束拒绝、重启验证和批量导入。
- [资料来源与核对记录](docs/SOURCES.md)：官方章节、文法附件和评测仓库。
- [文档审查记录](docs/REVIEW.zh-CN.md)：一次完整 review 的范围、修正项与验证结果。

文档中的接口、页格式和算法细节属于建议方案；课程约定单独标明。`testcase.yml` 已配置必做测例类别，但当前骨架还不能通过这些数据库功能测例。
