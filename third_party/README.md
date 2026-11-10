# 第三方依赖

catalog 模块使用仓库内的 nlohmann/json。实现 SQL 解析时再加入 ANTLR C++ runtime。

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
