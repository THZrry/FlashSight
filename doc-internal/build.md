# 构建与选型（build）

> 决策来源：`rules/user.md`、addons §10；QA Q14、Q17–Q18。

## 1. 工具链

- xmake + C++14 + reproc（子进程管理）。
- 环境记录：setup 阶段把 python/C++ 编译器版本等记入 `temp/environment.md`
  （见 rules-for-LLMs §Setup）。

## 2. targets（见 arch.md §3）

`fs_core`（静态库） / `fs_ui`（静态库，独立目录） /
`flashsight`（主程序） / `fs_tests`（单测）。

## 3. 第三方（定稿 + 待验证）

| 用途 | 选型 | 状态 |
|---|---|---|
| path/遍历 | ghsl filesystem（C++14，以后换 std） | 定稿 |
| HTTP client+server | cpp-httplib（header-only，利好以后 HTTP 分离）+ mbedTLS | TODO：开工前验证 Win/Linux 编译与 HTTPS，不可用回退 libcurl |
| SSH/远程 | libssh2（v1 预留，只给远程连接入口） | 预留 |
| markdown | md4c，fork 魔改输出 diff AST（tree-sitter 太重不用） | 定稿方向，魔改量实现时评估 |
| JSON | nlohmann/json（单头） | 定稿 |
| diff | dtl（单头，配合 diff 写） | 定稿 |
| git | 调 git CLI（零依赖起步） | 定稿 |
| 相似度 | 自研小函数（SimHash/归一化编辑距离） | 定稿，阈值联调再定 |
| search | 先调研开源实现，有则用，无则接可配 HTTP 搜索 API | TODO |

## 4. 测试先行（FsApi 定稿后写单测）

- 状态机（手动/自动/挂起/唤醒，时钟注入）。
- HistoryBuilder（窗口+压缩+遗忘+前缀顺序）。
- 权限矩阵（三档×锁×黑白名单+拒绝协议）。
- 回滚/分叉（patch 栈逆向 + 缺文件容忍）。
- 终端晋升（5min 规则，可注入时钟测）。
- 用量聚合（JSONL 坏行容忍）。
- 改完先自查再跑测试（rules-for-LLMs：Check first）。

## 5. 文档与笔记纪律

- doc-internal 9 篇：arch / session-history / agent-loop / permissions /
  terminal / provider / usage / ui-events / build（本文件）。
- 重要事项记 notes（编号递增、index/current 同步、tag 时归档）；
  工作区干净，临时数据只放 temp/。
