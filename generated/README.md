# 解析器生成目录

ANTLR 生成过程已接入 CMake，使用 `grammar/SQL.g4` 和固定版本 4.13.2 的工具/runtime。

实际输出位于各自构建目录的 `generated/sql/`，例如 `build-debug/generated/sql/`，
其中包含 `SQLLexer`、`SQLParser`、`SQLBaseVisitor`、`SQLVisitor` 的 `.h/.cpp`，
以及 `.tokens/.interp`。Debug/Release 构建互不覆盖。

源码树的这个目录只保留说明。生成产物不提交；具体步骤见 [tools/README.md](../tools/README.md)。
手写 AST、Visitor 业务代码仍放在 `include/db/parser/` 和 `src/parser/`。
