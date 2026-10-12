# 开发工具

当前基础构建只编译 CLI，不需要 Java 或 ANTLR。实现解析器时，先准备版本兼容的 ANTLR 工具和 C++ runtime，并把实际版本记录到 `third_party/README.md`。

把选定的工具 jar 放在 `tools/antlr4.jar` 后，可从仓库根目录运行以下命令。进入文法目录生成，避免输入路径使输出额外嵌套 `grammar/`：

```bash
cd grammar
java -jar ../tools/antlr4.jar -Dlanguage=Cpp -visitor -no-listener -o ../generated SQL.g4
cd ..
```

jar 不随骨架提供。生成后，检查 `generated/` 的实际文件，再添加 runtime 依赖、解析器 CMake 目标和错误处理。生成代码不会自动加入现有 `mydb`。

之后可在这里增加 `dump-page`、`dump-tree` 等调试工具，工具的进度输出写 stderr，避免污染 DBMS 批处理 stdout。
