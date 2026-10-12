# 资料来源与核对记录

核对日期：2026-10-09。开发者手册以官方教程为需求来源，以官方附件和评测器核对语法与入口；接口、二进制布局、伪代码、示例数据、测试矩阵和开发建议为本手册自行组织的参考方案。

## 官方教程

- [实验首页](https://thu-db.github.io/dbs-tutorial/)与 [0.1 系统概述](https://thu-db.github.io/dbs-tutorial/chapter-0/0-1-intro/)：项目范围、模块和附件。
- [0.2 后端约定](https://thu-db.github.io/dbs-tutorial/chapter-0/0-2-backend/)：关系模型举例与展示语法边界。
- [0.3 前端约定](https://thu-db.github.io/dbs-tutorial/chapter-0/0-3-frontend/)：输入输出、导入、DESC。
- [1.1 文件管理概述](https://thu-db.github.io/dbs-tutorial/chapter-1/1-1-intro/)、[1.2 页式文件系统](https://thu-db.github.io/dbs-tutorial/chapter-1/1-2-page/)、[1.3 缓存管理](https://thu-db.github.io/dbs-tutorial/chapter-1/1-3-buffer/)、[1.4 接口](https://thu-db.github.io/dbs-tutorial/chapter-1/1-4-api/)：页、文件 I/O、缓存策略。
- [2.1 记录概述](https://thu-db.github.io/dbs-tutorial/chapter-2/intro/)、[2.2 定长记录](https://thu-db.github.io/dbs-tutorial/chapter-2/fixed/)、[2.3 变长记录](https://thu-db.github.io/dbs-tutorial/chapter-2/variable/)：类型、记录宽度、槽位与空间管理。
- [3.1 索引概述](https://thu-db.github.io/dbs-tutorial/chapter-3/intro/)、[3.2 B+ 树](https://thu-db.github.io/dbs-tutorial/chapter-3/btree/)、[3.3 其他索引](https://thu-db.github.io/dbs-tutorial/chapter-3/ds/)：基础索引范围与算法约定。
- [4.1 解析器](https://thu-db.github.io/dbs-tutorial/chapter-4/4_1_intro/)、[4.2 SQL 与 ANTLR](https://thu-db.github.io/dbs-tutorial/chapter-4/4_2_sql/)、[4.3 逻辑优化](https://thu-db.github.io/dbs-tutorial/chapter-4/4_3_logic/)、[4.4 物理优化](https://thu-db.github.io/dbs-tutorial/chapter-4/4_4_physic/)：解析和优化的职责、选做范围。
- [5.1 系统管理](https://thu-db.github.io/dbs-tutorial/chapter-5/5-1-intro/)、[5.2 数据库管理](https://thu-db.github.io/dbs-tutorial/chapter-5/5-2-db/)、[5.3 表管理](https://thu-db.github.io/dbs-tutorial/chapter-5/5-3-table/)：目录、schema 与约束修改。
- [6.1 查询处理](https://thu-db.github.io/dbs-tutorial/chapter-6/6-1-overview/)、[6.2 扫描](https://thu-db.github.io/dbs-tutorial/chapter-6/6-2-scan/)、[6.3 连接](https://thu-db.github.io/dbs-tutorial/chapter-6/6-3-join/)、[6.4 执行](https://thu-db.github.io/dbs-tutorial/chapter-6/6-4-execution/)：扫描、连接、物化与流水线。
- [文法总述](https://thu-db.github.io/dbs-tutorial/explanation/intro/)、[数据库文法](https://thu-db.github.io/dbs-tutorial/explanation/db/)、[表管理文法](https://thu-db.github.io/dbs-tutorial/explanation/table/)、[查询文法](https://thu-db.github.io/dbs-tutorial/explanation/query/)：合法操作、非法输入和扩展边界。
- [CI 整体说明](https://thu-db.github.io/dbs-tutorial/ci/intro/)、[平台](https://thu-db.github.io/dbs-tutorial/ci/platform/)、[运行要求](https://thu-db.github.io/dbs-tutorial/ci/run/)、[环境](https://thu-db.github.io/dbs-tutorial/ci/env/)、[本地测试](https://thu-db.github.io/dbs-tutorial/ci/local/)、[测例格式](https://thu-db.github.io/dbs-tutorial/ci/testcase/)、[参数](https://thu-db.github.io/dbs-tutorial/ci/args/)：评测与资源。
- [FAQ](https://thu-db.github.io/dbs-tutorial/faq/)、[附件](https://thu-db.github.io/dbs-tutorial/files/)、[更新记录](https://thu-db.github.io/dbs-tutorial/changelog/)：参考代码注意点与变化。
- [2026 秋安排](https://thu-db.github.io/dbs-tutorial/timeline/2026F/)：仅用于核对当前资料时间；具体期限以课程通知为准。

## 文法与评测器

2026-10-12 工程接入时再次下载官方 `SQL.g4`，保存于仓库 `grammar/SQL.g4`，SHA-256 与下表原核对版本相同；同时下载核对 `run-ci.py` 的 `compile`/`run` 配置字段和 `testcase.yml` 入口。此次没有运行官方数据库功能评测，当前 CLI 骨架的实际能力见 [构建说明](BUILD.zh-CN.md)。

- [SQL.g4 官方发布附件](https://github.com/thu-db/dbs-tutorial/releases/latest/download/SQL.g4)：下载并核对了实际规则。`latest` 会随发布变化。
- [DBS-Testcase README](https://github.com/thu-db/dbs-testcase/blob/main/README.md)：runner 参数、flags、工作目录与 YAML 用法。
- [run-ci.py](https://github.com/thu-db/dbs-testcase/blob/main/run-ci.py)：实际配置入口 `testcase.yml` 与启动封装。
- [runner.py](https://github.com/thu-db/dbs-testcase/blob/main/runner.py)：`-f`、`-c`、`--` 等参数边界。

本次资料下载的 SHA-256 用于识别参考版本，不是对将来版本的约束：

| 资料 | SHA-256 |
| --- | --- |
| 官方站点 `search/search_index.json` | `088c6c7eaa6071c61ac4114f677456291734437d65d65488ba6010ae0f9cd5e9` |
| 官方附件 `SQL.g4` | `56636fced8fe7fc3c744e700ab75bac6e6ebca24f87b5e5c34aa99d4a8a6fb69` |
| 测例仓库 `README.md` | `f44acc2dc0c52675f84eb0f6c6c5a8ff2be6feafa629b60c3c41a9af999ec90e` |
| 测例仓库 `run-ci.py` | `4b9138a5d2a0315929b80562abc27efc312ad21aa4ba9e5ca5991e4aaafeea72` |
| 测例仓库 `runner.py` | `a0b7fe9919321ed5109e998ea13e7a0d8a76449d6f8ba3e68368bef521ce99bc` |

## 对原资料的使用方式

手册没有照搬官方教程正文或参考文件系统源码。它保留来源链接并独立展开实现建议；发现展示示例、文字说明和实际规则不同之处时，在相关章节给出具体边界。

关键补充包括：区别页号与缓存帧号、使用 pin/guard 防止缓存引用失效、精确计算槽位图开销、处理重复索引键、维护 B+ 树最大键、已有数据上的约束验证、更新扫描反馈、有界内存连接及导入、及时刷新批处理输出。

资料升级后，优先重新核对前端约定、`SQL.g4`、测例 flags、环境与实际 CI 入口，再更新手册和自编示例。

<a id="本次文档校核"></a>
## 本次文档校核

- Markdown 渲染后检查 50 处本地链接与章节跳转，无缺失目标，代码围栏成对；正文 22 章编号连续。83 处官方教程页面链接均在下载的站点索引中存在。
- 按下载附件的解析规则，并以覆盖示例输入的词法识别器，核对了 7 个 SQL 文件和正文 4 个 SQL 代码块；此核对没有调用 ANTLR 生成器或 C++ runtime。
- 用 SQLite 作为基础关系语义参考，适配数据库切换、索引 DDL、课程错误标签和 UPDATE 修改数后，核对了 mvp、smoke、constraints、restart、import-verify 共 44 个结果块及 3 行 CSV。重复添加主键的课程错误由参考校核显式建模。
- 复核了记录槽容量与索引节点容量计算，并解析 YAML 配置草图，确认 flags 为字符串列表。

这些检查验证文档内部一致性和自编示例的参考结果，不验证学生 DBMS 的页布局、B+ 树、导入入口、正常退出持久化或 CI 通过情况。上述运行验证必须在实际引擎完成后执行。

完整 review 的发现、修正和结论见 [文档审查记录](REVIEW.zh-CN.md)。
