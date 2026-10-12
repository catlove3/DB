# 解析器生成目录

当前只有原始文法 `grammar/SQL.g4`，尚未生成解析器，也尚未链接 ANTLR runtime。

本工程选择“本地或构建时生成，生成产物不提交”的方式。生成步骤见 [tools/README.md](../tools/README.md)。接入构建时应声明生成目标的依赖，并让 `db_parser` 编译实际生成的源码。

此目录中的生成文件已由根 `.gitignore` 忽略；不要把手写 AST、Visitor 业务代码放在这里，应放到 `include/db/parser/` 和 `src/parser/`。
