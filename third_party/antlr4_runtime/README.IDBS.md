# ANTLR C++ runtime 本地集成

- 版本：4.13.2。
- 来源：[官方 C++ 源码分发包](https://www.antlr.org/download/antlr4-cpp-runtime-4.13.2-source.zip)。
- 原始 ZIP SHA-256：`0ed13668906e86dbc0dcddf30fdee68c10203dea4e83852b4edb810821bee3c4`。
- 许可证：BSD-3-Clause，见 `LICENSE.txt`。
- 保留原包的 `runtime/src/`、`runtime/CMakeLists.txt`、`cmake/`、顶层 CMake、版本、说明和许可证；未收录 demo、上游测试及 IDE 工程。
- CMake 关闭上游测试、demo 和动态库，只编译静态 runtime；不会下载 GoogleTest。

## 本地补丁

原包的 `runtime/src/Version.h` 定义了 `ANTLRCPP_VERSION_MAJOR/MINOR/PATCH`，
但组合数值和字符串时误用了不存在的 `ANTLR4CPP_VERSION_MAJOR/MINOR/PATCH`。
这会让 `RuntimeMetaData::getRuntimeVersion()` 返回宏名文本，而非版本号。
[上游 4.13.2 文件](https://github.com/antlr/antlr4/blob/4.13.2/runtime/Cpp/runtime/src/Version.h)
也存在该问题。

本地仅将这六处引用改为已经定义的 `ANTLRCPP_VERSION_*`；保留 `ANTLR4CPP_STRINGIFY`
不变，因为它本来就是正确的宏名。`unit_sql_grammar` 同时验证编译期版本值
`413002` 和运行时字符串 `4.13.2`。除此之外，所收录的上游文件保持原样。
