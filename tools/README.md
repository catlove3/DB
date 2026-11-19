# 开发工具

ANTLR 生成工具固定为 **4.13.2**，文件是 `antlr-4.13.2-complete.jar`，与仓库中的 C++ runtime 版本一致。
工具许可证见 [antlr-LICENSE.txt](antlr-LICENSE.txt)，来源与 SHA-256 见 [依赖记录](../third_party/README.md)。
jar 内包含的 ANTLR 3 runtime、StringTemplate 和 TreeLayout 许可证见
[第三方声明](antlr-THIRD-PARTY-NOTICES.txt)。

## Java 环境

生成器需要 Java 11+；运行编译好的 C++ 程序不需要 Java。Ubuntu/WSL 可以安装：

```bash
sudo apt-get install openjdk-17-jre-headless
java -version
```

CMake 优先查找系统 Java。也可以显式指定：

```bash
cmake --preset debug -DJava_JAVA_EXECUTABLE=/path/to/jre/bin/java
```

没有系统 Java 时，CMake 也会检查项目本地 `tools/.java/bin/java`。这个目录用于本机准备的
便携 JRE，已被 Git 忽略，不随仓库分发；CMake 不会自动下载安装 Java。

## 自动生成与编译

从仓库根目录执行：

```bash
cmake --preset debug
cmake --build --preset debug --parallel 2
ctest --preset debug
```

生成由 `cmake/Antlr.cmake` 管理，文法或 jar 更新后自动重新生成。仅生成代码可执行：

```bash
cmake --build build-debug --target generate_sql_parser
```

实际生成命令等价于：

```bash
cd grammar
java -jar ../tools/antlr-4.13.2-complete.jar \
  -Dlanguage=Cpp -visitor -no-listener -Werror \
  -o ../build-debug/generated/sql SQL.g4
cd ..
```

生成文件不提交，也不要手工修改。`db_sql_grammar` 编译生成代码并链接静态 runtime；
`db_parser` 私有依赖它，公共 `parser.h` 不包含 ANTLR 头文件。
`unit_sql_grammar` 验证版本、合法/非法 SQL、错误位置和生成的 Visitor 分派。

之后可在这里增加 `dump-page`、`dump-tree` 等调试工具，工具的进度输出写 stderr，避免污染 DBMS 批处理 stdout。
