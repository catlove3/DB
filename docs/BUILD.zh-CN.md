# 构建与工程结构

本文说明仓库当前实际存在的工程。完整 DBMS 的接口和实现建议见 [开发者手册](DEVELOPER_GUIDE.zh-CN.md)；手册中的未来能力不代表当前已经实现。

## 1. 工程根目录

原 `project/` 中的骨架已提升至仓库根目录，空的重复文档目录已合并。所有命令从 `IDBS/` 执行，课程封装脚本也从这里读取 `testcase.yml`。目录拼写统一为 `execution`。

```text
IDBS/
├── CMakeLists.txt                 # 构建入口
├── CMakePresets.json              # Debug/Release 预设
├── testcase.yml                  # 课程评测配置
├── include/db/                   # common/storage/record/index/catalog/parser/execution
├── src/
│   ├── main.cpp                  # 进程入口与临时 shell
│   └── common/cli.cpp            # 参数解析
├── grammar/SQL.g4                # 官方原始文法，已固定文件内容
├── generated/                    # 后续生成的解析器代码，不提交生成产物
├── third_party/                  # 后续依赖源码、版本与许可证说明
├── tools/                        # 后续生成工具、页与树调试工具
├── tests/{unit,integration,sql}/
├── docs/
├── examples/                     # 供实际 DBMS 完成后使用
└── data/                         # 运行时数据，不提交
```

尚未有源码的模块目录通过 `.gitkeep` 保留，避免 clone 后目录消失。`data/` 整体忽略，clone 后可能不存在；未来由数据初始化逻辑创建。

| 模块 | 接下来应承担的职责 |
| --- | --- |
| `common` | 公共类型、错误、二进制编码；当前已有 CLI 参数解析 |
| `storage` | 文件读写、页号、缓存帧、pin/guard 和脏页刷写 |
| `record` | schema、槽位图、RID、记录插入与扫描 |
| `index` | B+ 树节点布局、查找、分裂和索引维护 |
| `catalog` | 数据库与表目录、元信息持久化、约束定义 |
| `parser` | 官方文法解析、错误收集、独立 AST |
| `execution` | 表扫描、过滤、连接、修改与结果格式化 |

## 2. Linux 构建与验证

基础构建仅需要 CMake 3.21+ 和支持 C++17 的编译器。Python 3 用于进程级 CLI 测试；没有 Python 时仍可构建程序，配置阶段会提示测试未启用。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure --no-tests=error
./build/mydb --help
./build/mydb
```

也可选择独立构建目录的预设：

```bash
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug
./build-debug/mydb
```

将 `debug` 替换为 `release` 即构建 Release，对应程序在 `build-release/mydb`。`examples/` 和 `testcase.yml` 使用 `build/mydb`，执行它们时采用第一组命令，或明确修改可执行文件路径。

编译器警告按目标设置：GCC/Clang 使用 `-Wall -Wextra -Wpedantic`，MSVC 使用 `/W4 /permissive- /utf-8`。C++17 已声明，关闭编译器语言扩展。当前不自动下载依赖，基础构建可离线完成。

## 3. Visual Studio

在支持 CMake 的 Visual Studio 中使用“打开文件夹”，选择包含根 `CMakeLists.txt` 的 `IDBS/`。若安装界面提供组件选择，需要 C++ 编译工具和 CMake 支持。选择 `debug` 或 `release` 预设，再选择 `mydb` 目标构建和运行。

使用 Visual Studio 多配置生成器时，程序通常位于 `build-debug/Debug/mydb.exe` 或 `build-release/Release/mydb.exe`；使用单配置 Ninja 时通常直接位于构建目录。以 IDE 展示的目标路径为准。预设已为构建和测试指定对应配置。

原 `数据库大作业/` 下的 `.sln` 和 `.vcxproj` 保留，作为原来的空项目资料；它们没有关联新增 CMake 源码。日常开发使用根 CMake 工程，不需要同时维护两套源码列表。`.vs/` 和 `*.vcxproj.user` 已加入忽略规则。本次实际编译验证在 Linux/GCC 环境进行，尚未实测 MSVC。

## 4. 当前 CLI 的能力

- `-h`/`--help`：展示用法后退出。
- `-b`/`--batch`：无启动横幅和提示符；每个非空输入行暂时视为一次 SQL 请求。
- `exit`、`exit;` 和 EOF：正常结束；空白行忽略。
- SQL 输入：当前输出 `!ERROR`、功能未实现说明和 `@`，并立即 flush。
- `--init`：当前没有持久化状态，直接以 0 退出，不读取 SQL，不创建或删除数据。后续接入存储时必须替换为课程要求的数据初始化流程。
- `--data_dir PATH`：解析并保存参数，当前尚未打开数据目录。
- `-d`/`--database`：参数已识别，但数据库选择尚未实现，会非零退出。
- `-f`/`--file` 与 `-t`/`--table`：检查成对使用；导入尚未实现，会非零退出。
- 未知参数、缺失值或冲突组合：向 stderr 输出错误，并非零退出。

课程批处理每行一个完整 SQL 的约定见 [运行要求](https://thu-db.github.io/dbs-tutorial/ci/run/)；完整参数行为见 [命令行参数](https://thu-db.github.io/dbs-tutorial/ci/args/)。多行 SQL、词法拆分和语法验证须在接入真正的解析器后实现，当前 shell 不承担这些职责。

## 5. 添加实现与测试

头文件放在 `include/db/<模块>/`，实现放在 `src/<模块>/`。开始写存储时，在根 CMake 中增加实际源码目标，例如：

```cmake
# 下面文件需先实现；这是接入方式示例，不是当前已有目标。
add_library(db_storage STATIC src/storage/file_manager.cpp)
target_include_directories(db_storage PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(db_storage PUBLIC db_common)
idbs_configure_target(db_storage)
target_link_libraries(mydb PRIVATE db_storage)
```

明确列出源码，新增文件时同步修改 CMake。先做文件管理模块的小闭环：创建文件、写完整页、关闭、重新打开、校验字节一致。再接缓存管理和记录管理。

当前 CTest 的 `cli_integration` 是一个集成测试入口，内部验证启动、帮助、批处理输出、及时刷新、退出、参数错误，以及骨架初始化不修改文件。它不验证页、记录、SQL 语义或持久化。未来单元测试放 `tests/unit/`，进程集成测试放 `tests/integration/`，SQL 测例放 `tests/sql/`；按模块逐步加入 CTest。

2026-10-12 在 Linux/GCC 13.3、CMake 3.28 环境完成 Debug 构建和全部 9 项 CLI 检查，并实际执行 `testcase.yml` 中的 Release 编译命令，核对该程序的初始化退出和批处理退出。课程构建关闭 `BUILD_TESTING`；上面的本地命令显式重新开启它，避免复用缓存后没有测试。

## 6. 文法与课程评测

`grammar/SQL.g4` 来自 [官方附件](https://thu-db.github.io/dbs-tutorial/files/)，下载日期为 2026-10-12，SHA-256 为 `56636fced8fe7fc3c744e700ab75bac6e6ebca24f87b5e5c34aa99d4a8a6fb69`，与手册原核对版本相同。

当前没有生成或链接解析器。先选定相同兼容版本的 ANTLR 工具和 C++ runtime，再按 [工具说明](../tools/README.md) 生成代码，并增加解析器构建目标。将依赖来源、版本、哈希与许可证放入 `third_party/`，保证课程构建环境能离线取得这些依赖。

`testcase.yml` 的结构已对照 [官方 run-ci.py](https://github.com/thu-db/dbs-testcase/blob/main/run-ci.py) 核对，编译和运行命令均以仓库根目录为基准。必做 flags 为 `query/data/index/join/pk/fk/comb`，它们列出后续验收范围，不表示当前骨架能够通过测例。原课程 CI 入口和评测器应使用教学组提供的版本。

`.gitignore` 已排除构建目录、IDE 缓存、数据文件和生成产物；示例 CSV、文档、源码、官方文法和工程配置保留在版本控制范围内。
