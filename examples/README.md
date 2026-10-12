# SQL 示例与预期结果

这些文件用于完成 DBMS 后验证开发手册中的设计。工作区已提供可编译的 `mydb` 命令行骨架，但尚未实现 SQL、存储和导入，当前不能通过下面的示例。采用根 README 的 `build/` 构建方式后，程序路径就是 `./build/mydb`；使用其他构建目录时替换该路径。

示例按 2026-10-09 下载的官方 `SQL.g4` 编写：大写关键字、表级主外键、全列 INSERT、隐式连接。所有基础 SQL 不依赖聚合、排序、嵌套查询等选做。

## M3 最小闭环

[mvp.sql](mvp.sql) 与 [mvp.expected](mvp.expected) 用于刚完成数据库、表、记录和全表扫描的阶段；不依赖索引、主外键或连接。使用独立数据目录：

```bash
./build/mydb -b --data_dir ./test-data-mvp --init
./build/mydb -b --data_dir ./test-data-mvp < examples/mvp.sql > /tmp/idbs-mvp.actual
diff -u examples/mvp.expected /tmp/idbs-mvp.actual
```

完成后再接入后面的基础集成示例。所有命令从学生项目根目录执行；`--data_dir` 是手册建议实现的参数。

## 基础闭环

从项目根目录执行，`--data_dir` 是手册建议实现的参数：

```bash
./build/mydb -b --data_dir ./test-data --init
./build/mydb -b --data_dir ./test-data < examples/smoke.sql > /tmp/idbs-smoke.actual
diff -u examples/smoke.expected /tmp/idbs-smoke.actual
```

[smoke.sql](smoke.sql) 创建 campus 数据库和三张表，覆盖多行插入、显式索引、查询、连接、未变化更新、实际变化更新、删除和空结果。[smoke.expected](smoke.expected) 中每个查询至多一行，因此不依赖未指定的结果行序。

该 diff 适用于示例规定的表头、无附加注释的 `@` 和相同 CSV 风格。课程允许的表限定名前缀、分隔行注释等变化可能导致文本 diff 不同；正式验收应使用官方评测器或符合其规则的结果比较。

基础脚本完成后：student 只剩 Alice，gpa 为 3.90；enrollment 剩 Alice 的两门课；course 仍有两行。

## 约束拒绝

在基础脚本的状态上运行：

```bash
./build/mydb -b --data_dir ./test-data -d campus < examples/constraints.sql
```

[constraints.sql](constraints.sql) 验证重复主键、缺失父键、删除被引用父行、修改被引用父键和重复添加主键。[constraints.expected](constraints.expected) 的错误文字只是参考；检查 `!ERROR` 与对应的 `duplicate`、`foreign`、`primary`，无需逐字比较错误句子。

所有非法操作都应被拒绝；后续 SELECT 必须仍能看到原有 Alice，不能出现 id=3 的学生或缺失的索引项。

## 正常退出与重启

基础脚本通过输入重定向产生 EOF 并退出。再次启动：

```bash
./build/mydb -b --data_dir ./test-data -d campus < examples/restart.sql
```

[restart.sql](restart.sql) 与 [restart.expected](restart.expected) 验证更新后的值、enrollment、age 索引可用的数据，以及恢复后的主外键拒绝。结果正确不能单独证明查询使用了索引，应结合执行计划或读页计数确认。

重启验证之前不能再次执行 `--init`。若要额外测试 `exit`，用交互或管道在完整语句之后输入该命令，再启动读取相同数据。

## 比较两种导入入口

下面采用两个独立数据目录，避免同一文件重复导入导致主键冲突。示例 CSV 为无表头的全列数据，gpa 输入列为双精度数值。

命令行导入：

```bash
./build/mydb -b --data_dir ./test-data-cli --init
./build/mydb -b --data_dir ./test-data-cli < examples/import-bootstrap.sql
./build/mydb -b --data_dir ./test-data-cli -d import_demo -t student -f examples/students.csv
./build/mydb -b --data_dir ./test-data-cli -d import_demo < examples/import-verify.sql
```

SQL 导入：

```bash
./build/mydb -b --data_dir ./test-data-sql --init
./build/mydb -b --data_dir ./test-data-sql < examples/import-bootstrap.sql
./build/mydb -b --data_dir ./test-data-sql -d import_demo < examples/import-load.sql
./build/mydb -b --data_dir ./test-data-sql -d import_demo < examples/import-verify.sql
```

两种入口都应导入 3 行；查询 [import-verify.sql](import-verify.sql) 应得到 [import-verify.expected](import-verify.expected) 的相同多重集，第二个查询的 Bob、Cara 行序可以交换。age 是建好的显式索引，应在导入后仍可正确使用。

CSV 文件与结果文件的具体内容均为本手册自编示例；正式导入数据格式和测例请参照教学组资料。官方本地测试方法见 [手册测试章节](../docs/DEVELOPER_GUIDE.zh-CN.md#测试与验收)。
