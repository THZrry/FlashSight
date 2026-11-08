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

## 2.1 三个 UI 变体（独立开关 + build_all.py）

```sh
xmake f --variant=web      -y --toolchain=mingw && xmake b   # 默认，单变体
xmake f --variant=webview  -y --toolchain=mingw && xmake b
xmake f --variant=whaleui  -y --toolchain=mingw && xmake b
# 一次编译多个（脚本按变体逐个 configure/build；项目级 xmake.lua 不能注册 task）。
# xmake f 里也暴露了同名开关 --web/--webview/--whaleui 供配置查看/记录，
# 但真正一次跑多个变体仍由脚本完成：
python scripts/build_all.py --web --webview -- --toolchain=mingw --mingw="C:/Program Files/mingw64"
```

`--variant` 仍然只负责**单变体**的 target 图：web 可执行文件和 WebView 窗口是不同
二进制，一个 xmake target 不能同时链接两个 host。`build_all.py` 用同一个输出目录
逐个构建，产物名带变体，互不覆盖；不带参数时默认 web+webview。

产物都在同一个输出目录，**文件名带变体**：
`flashsight-web[.exe]` / `flashsight-webview[.exe]` / `flashsight-whaleui[.exe]`。

单变体构建仍然用 `--variant`；多选编译由 `scripts/build_all.py` 的命令行开关选择
变体并逐个 configure/build。`xmake f --web/--webview/--whaleui` 只是把同一组
变体意图记录在配置里，单个 target 图不会因此变成三个可执行文件。

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
用 `--variant=web` 和 `--variant=webview` 各编一次（同一个输出目录，切变体只
重编三个文件，所以放在一个 job 里而不是两个），`scripts/package.py` 按变体名
找可执行文件——**找不到就报错并提示该编哪个变体，绝不退化成"随便找个
flashsight*"**，那个 fallback 正是错的二进制进发布包的原因。

构建后还有一道断言：二进制里必须真的有内嵌资源（`#fs-root`、样式、Web 版的
`fs_ui.wasm`），且 webview 版**不能**带 wasm（带了说明用错了变体的 pack）。
这类问题发布之后完全看不出来，只能在 CI 里拦。断言脚本按
`build/<plat>/<arch>/release` 查找，可执行文件名按平台带不带 `.exe`，所以
macOS / Linux 的 job 也适用。

`workflow_dispatch` 用三个独立的布尔开关（`web` / `webview` / `whaleui`）选择
编译哪些变体，而不是一个三选一的下拉：可以只编 webview 迭代，也可以两个都编。
`whaleui` 开关还要同时设置仓库变量 `ENABLE_WHALEUI=true` 才会生效。

WhaleUI 是**可选的独立 job**（`whaleui`），默认跳过：它需要 clone WhaleUI
仓库、先用它自己的 xmake 构建 `libwhaleui-full`，再让 xmake 装 lexbor /
utf8proc / SDL3 / SDL3_ttf / SDL3_image 这些 xrepo 依赖——是另一个仓库、另一
套依赖，时间也长得多。要打开就把仓库变量 `ENABLE_WHALEUI` 设为 `true`（换
fork 用 `WHALEUI_REPO`）。release job 用 `always() + result 检查`，所以它被跳
过时不会挡住发布，跑了就会等它并把它的包一起挂上去。

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
