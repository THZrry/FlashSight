# 构建与选型（build）

> 决策来源：`rules/user.md`、addons §10；QA Q14、Q17–Q18。

## 1. 工具链

- xmake + C++14 + reproc（子进程管理）。
- 环境记录：setup 阶段把 python/C++ 编译器版本等记入 `temp/environment.md`
  （见 rules-for-LLMs §Setup）。

## 2. targets（见 arch.md §3）

`fs_core`（静态库） / `fs_ui`（静态库，独立目录） /
`flashsight`（主程序） / `fs_tests`（单测）。Web 变体另有 `fs_ui_wasm`
（emcc，产物被 `fs_ui` 的 `before_build` 打包进 `ui_assets_gen.hpp`）；
Webview 变体另有 `webview_host`。

## 2.1 三个 UI 变体（一个单值选项，没有外部脚本）

```sh
xmake f --variant=web      -y --toolchain=mingw && xmake b   # 默认
xmake f --variant=webview  -y --toolchain=mingw && xmake b
xmake f --variant=whaleui  -y --toolchain=mingw && xmake b
xmake f --variant=web      -y --toolchain=mingw && xmake b && build/windows/x64/release/fs_tests.exe
```

产物都在同一个输出目录，**文件名带变体**：
`flashsight-web[.exe]` / `flashsight-webview[.exe]` / `flashsight-whaleui[.exe]`。

为什么不是两个布尔开关（`--webview` / `--whaleui`）：布尔值会一起留在
`.xmake` 缓存里，`xmake f --whaleui=y` 并不会清掉 `--webview`，于是"选了
WhaleUI 却出来 Web 包"。单值选项不可能自相矛盾。之前还有一个 `build.bat`
在外面切输出目录，那是多余的一层——xmake 里就能表达，而且脚本自己会和缓存
配置漂移。

为什么切变体只重编几个文件：变体宏（`FS_UI_WHALEUI` / `FS_UI_WEBVIEW`）
**不再项目级 `add_defines`**，而是只加在真正分叉的目标上：

| 目标 | 内容 | 变体宏 |
|---|---|---|
| `fs_ui` | 共享 UI（app/components/vdom/server…） | 无 |
| `fs_ui_host` | 选中变体的两个后端文件 | 有 |
| `flashsight` | `main.cpp` | 有 |
| `fs_ui_wasm` | 仅 `--variant=web` | — |

项目级宏会进入每个编译单元的命令行，改一次就等于全量重编；现在 `web ↔
webview` 只重编 `ui_server.cpp`（资源头变了）+ 两个后端 + `main.cpp`，
实测约 7 秒。

生成头放在 `build/gen/`（不是 `src/`）：以前写进 `src/ui/` 且所有变体共用
一个文件名，两个变体的 pack 会互相覆盖，桌面版可能拿到 Web 版的包（这就是
"webview 资源没嵌入"的直接原因）。目录**跨变体固定**，否则 include 路径变化
同样会让整个 `fs_ui` 重编。

## 2.2 CI 与发布包

`.github/workflows/build.yml` 按 tag（`v*`）与 `release: published` 触发，
矩阵覆盖 Windows x86_64/arm64、macOS arm64、Linux x86_64/aarch64；每个 job
用 `--variant=web` 和 `--variant=webview` 各编一次，`scripts/package.py`
按变体名找可执行文件（找不到就报错并提示该编哪个变体，绝不退化成"随便找个
flashsight*"），出两个包并挂到 release。构建后还会断言二进制里确实有内嵌
资源（`#fs-root`、Web 版的 `fs_ui.wasm`），因为这两类问题在发布之后完全看不
出来。WhaleUI 不在 CI 里（需要 WhaleUI 仓库与其引擎构建产物）。

依赖：`scripts/fetch_3rdparty.py`（幂等，Windows/macOS/Linux 通用），
mbedtls 由 `mbedtls_src` 目标从源码编译（见文首说明）。

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
