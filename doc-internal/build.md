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

## 2.1 三个 UI 变体 = 三个 xmake target

干净 checkout 先拉一次依赖（`3rdparty/` 不入 git；缺文件时 `xmake f`
会在 configure 阶段直接列出缺的路径并提示这条命令）：

```sh
python scripts/fetch_3rdparty.py
```

```sh
# Windows: use the mingw PLATFORM, not only the mingw toolchain. This keeps
# cc/ld/windres on one toolchain for web and webview targets.
xmake f -y -p mingw -a x86_64 --mingw="C:/Program Files/mingw64"
# arm64 cross: xmake f -y -p mingw -a arm64 --mingw=C:/llvm-mingw/.../
xmake build flashsight-web          # browser + embedded HTTP server
xmake build flashsight-webview      # native window via the OS web engine
xmake build flashsight-whaleui      # native window via WhaleUI
# 编全部三个：每个 target 单独一次（xmake build 只接受一个 target，
# 第二个会被报 invalid argument）：
for t in flashsight-web flashsight-webview flashsight-whaleui; do
  xmake build "$t"
done
```

三个可执行文件是三个独立的 target，输出名互不覆盖。`flashsight-web` 只在
emcc 可用时定义；`flashsight-whaleui` 只在 `--whaleui_libdir` 里找得到
`libwhaleui-full.a` 时定义。没有 `--variant` 这种缓存单值，也没有外部
Python 脚本：一条 `xmake b <target>...` 就是全部。

产物都在同一个输出目录，**文件名带变体**：
`flashsight-web[.exe]` / `flashsight-webview[.exe]` / `flashsight-whaleui[.exe]`。

变体宏（`FS_UI_WHALEUI` / `FS_UI_WEBVIEW`）只加在对应 host library 和
各自 exe 的 `main.cpp` 上；共享 `fs_ui` 只编译一次：

| 目标 | 内容 | 变体宏 |
|---|---|---|
| `fs_ui` | 共享 UI（app/components/vdom/server…） | 无 |
| `fs_ui_host_webview` | ui_dom_webview.cpp + ui_main_webview.cpp | `FS_UI_WEBVIEW` |
| `fs_ui_host_whaleui` | ui_dom_whale.cpp + ui_main_whale.cpp | `FS_UI_WHALEUI` |
| `flashsight-web` | `main.cpp` + fs_ui + wasm bundle | 无 |
| `flashsight-webview` | `main.cpp` + webview host | `FS_UI_WEBVIEW` |
| `flashsight-whaleui` | `main.cpp` + WhaleUI host | `FS_UI_WHALEUI` |
| `fs_ui_wasm` | wasm UI bundle（emcc 可用时） | — |

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
一次 configure，按 target 名逐个构建选中的变体（`xmake build flashsight-web`
再 `xmake build flashsight-webview`），共享 `fs_ui` 只编译一次，`scripts/package.py` 按变体名
找可执行文件——**找不到就报错并提示该编哪个 target，绝不退化成"随便找个
flashsight*"**，那个 fallback 正是错的二进制进发布包的原因。

构建后还有一道断言：二进制里必须真的有内嵌资源（`#fs-root`、样式，Web 版的
`fs_ui.wasm`）。三个 target 共用同一个 `fs_ui` 和同一份资源 pack，因此桌面
二进制也会带 wasm 字节；它是无害的多余数据，不是"用错了变体"。断言脚本按
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
