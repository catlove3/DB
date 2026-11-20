# 第三方依赖

catalog 使用仓库内的 nlohmann/json；parser 的构建使用仓库内的 ANTLR 工具和 C++ runtime。

## ANTLR

- 固定版本：生成器和 runtime 均为 **4.13.2**。
- [官方发布](https://github.com/antlr/antlr4/releases/tag/4.13.2)、[下载页](https://www.antlr.org/download.html)。
- [生成工具 jar](https://www.antlr.org/download/antlr-4.13.2-complete.jar)，保存于 `tools/antlr-4.13.2-complete.jar`。
- [C++ runtime 源码 ZIP](https://www.antlr.org/download/antlr4-cpp-runtime-4.13.2-source.zip)，编译所需源码保存于 `antlr4_runtime/`。
- 许可证：BSD-3-Clause，完整文本见 `antlr4_runtime/LICENSE.txt` 和 `tools/antlr-LICENSE.txt`。
- jar 内嵌组件的版本、来源和许可证另见 `tools/antlr-THIRD-PARTY-NOTICES.txt`。
- 收录范围与唯一的版本宏修正见 [runtime 集成记录](antlr4_runtime/README.IDBS.md)。

| 文件 | SHA-256 |
| --- | --- |
| 原始生成工具 jar | `eae2dfa119a64327444672aff63e9ec35a20180dc5b8090b7a6ab85125df4d76` |
| 原始 runtime ZIP | `0ed13668906e86dbc0dcddf30fdee68c10203dea4e83852b4edb810821bee3c4` |
| 未修改的课程 `grammar/SQL.g4` | `56636fced8fe7fc3c744e700ab75bac6e6ebca24f87b5e5c34aa99d4a8a6fb69` |

CMake 校验 jar 哈希和 runtime 的 `VERSION`。Java 11+ 是外部构建前提，详见
[工具说明](../tools/README.md)。上游 demo、测试和动态库关闭，不会下载 GoogleTest；
runtime 和生成代码使用独立目标，不继承项目手写代码的警告选项。
生成文件保存在构建树中，`db_parser` 公开依赖 `db_sql_grammar`，让包含 `parser.h` 的目标
能使用 AST 转换成员声明中的 ANTLR 节点类型。

## nlohmann/json

- 固定版本：3.12.0。
- 官方发布：https://github.com/nlohmann/json/releases/tag/v3.12.0
- 原始头文件：https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp
- 原始许可证：https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT
- 本地头文件：`nlohmann_json/include/nlohmann/json.hpp`，保留上游原文。
- 许可证：MIT，全文位于 `nlohmann_json/LICENSE.MIT`。
- 头文件 SHA-256（与官方发布页一致）：`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`。

这是仅头文件库，不需要单独编译或系统安装。CMake 提供
`nlohmann_json::nlohmann_json` 接口目标，由 `db_catalog` 私有依赖。
在 `catalog.cpp` 中使用 `#include <nlohmann/json.hpp>`；公共头文件继续使用项目自己的类型。

所有依赖文件随仓库保存，以下配置和构建步骤无需下载：

```bash
cmake -S . -B build-offline -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-offline -j2
ctest --test-dir build-offline --output-on-failure
```

每项依赖应记录：名称、固定版本、官方来源、下载或提交哈希、许可证，以及课程环境中的离线构建方法。ANTLR 生成工具与 runtime 需使用兼容版本。源码使用独立 CMake 目标，不继承本工程针对自写代码设置的全部警告。

不要只提交本机编译好的库文件；课程验收应能从源码重建。新增依赖后，实际运行一次无需下载的干净构建。
