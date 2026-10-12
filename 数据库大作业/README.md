# 原 Visual Studio 项目

此目录保留原来的空 `.sln` / `.vcxproj` 工程资料。项目的实际源码和 CMake 构建入口现在位于仓库根目录，旧解决方案尚未关联这些源码。

使用 Visual Studio 开发时，选择“打开文件夹”并打开上一级 `IDBS/`，使用根 `CMakeLists.txt` 和 `CMakePresets.json` 构建 `mydb`。详见 [构建说明](../docs/BUILD.zh-CN.md#3-visual-studio)。

`.vs/` 和 `.vcxproj.user` 是本地 IDE 状态，已忽略；原文件没有删除。
