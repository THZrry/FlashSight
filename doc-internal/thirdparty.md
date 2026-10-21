# 第三方依赖与出处（3rdparty）

> rules 要求 3rdparty/ 不入 git；`scripts/fetch_3rdparty.py`（`.sh`/`.bat` 只是壳）
> 可重放全部拉取。本文件记录每个依赖的**出处、版本、许可**，供分发时合规与升级参考。

| 目录 | 组件 | 版本 | 许可 | 用途 |
|---|---|---|---|---|
| `filesystem-1.5.14/` | gulrak/filesystem (ghc) | v1.5.14 | MIT | C++14 的 std::filesystem 兼容层 |
| `json/` | nlohmann/json | v3.11.3 | MIT | JSON 读写（`Json` 类型） |
| `dtl-1.20/` | cubicdaiya/dtl | v1.20 | BSD-2 | diff（diffWrite 上下文匹配、diff 展示） |
| `md4c-master/` | mity/md4c | master | MIT | Markdown 解析（UI 高亮/渲染） |
| `httplib/` | yhirose/cpp-httplib | master | MIT | HTTP(S) 客户端（NetClient） |
| `mbedtls-3.6.4/` | Mbed-TLS/mbedtls | v3.6.4 LTS | Apache-2.0 | TLS（httplib 的 mbedTLS 后端） |
| `qjs-regex/` | QuickJS regex engine | 2026-06-04 | MIT | `file.regex` 工具的正则引擎 |
| `reproc/` | DaanDeMeyer/reproc | v14.2.5 | MIT | 子进程管理（terminal.spawn/exec，rules/user.md） |
| `webview/` | webview/webview | 0.12.0（stable tag） | MIT | 备用 UI 宿主：原生窗口 + 系统 web 引擎（Windows = WebView2） |
| `mswebview2/` | Microsoft.Web.WebView2 SDK | latest nuget | Microsoft（随 SDK 分发） | webview 的 Windows 后端头文件（`WebView2.h`） |
| `whaleui/` | THZrry/WhaleUI | include/ 副本 | 见上游 | 主 UI 后端（`--whaleui=y`） |

## mbedtls 明细（TLS）

- **只用源码**：xmake 的 `mbedtls_src` target 直接编 `library/*.c`（C99），
  用项目自己的工具链。默认 `mbedtls_config.h` 不开 everest / p256-m 驱动，
  所以 `3rdparty/` 下那两个目录不参与编译。
- **不要用预编译的 cmake 产物**：本机曾用 MSVC 编过一份 `.a`，链进 MinGW
  程序后两套 CRT 混用（mbedtls/httplib 在一个堆上分配、在另一个堆上释放），
  第一次 HTTPS 请求就在 `RtlFreeHeap` 里 SIGSEGV。用同一套工具链编源码，
  分配与释放才对称。
- **httplib 的 TLS 宏必须是项目级的**（`CPPHTTPLIB_MBEDTLS_SUPPORT`）：它定义
  在头文件里，`ui_server.cpp`（fs_ui）和 `fs_net_http.cpp`（fs_core）都 include
  了 httplib.h。只在 fs_core 上定义会生成两份内联实现，链接器可能选中不带
  TLS 的那份，`SSLClient` 就退化成普通连接（表现为 "Could not establish
  connection"，而同一进程里另写一段请求却能成功）。

## reproc 明细（子进程管理）
- 只用 C 源码与头文件：`src/*.c|*.h` + `include/reproc/*.h`；平台相关文件
  （`*.windows.c` / `*.posix.c`）由 `xmake.lua` 按目标 OS 选择，不混编。
- 来源：GitHub release tarball `v14.2.5`（`scripts/fetch_3rdparty.*` 拉取）。
- 许可：MIT（见 `3rdparty/reproc/LICENSE`）。
- 集成点：`src/core/terminal_registry.cpp` + `src/tools/term_tools.cpp` 的
  spawn/poll/kill 回调（由 FsApi 注入 AgentHost，见 doc-internal/terminal.md）。

## qjs-regex 明细（addon2：regex 工具）

- 文件：`libregexp.c`、`libregexp.h`、`libregexp-opcode.h`、`libunicode.c`、
  `libunicode.h`、`libunicode-table.h`、`cutils.h`、`cutils.c`。
- 来源：QuickJS 发行包 2026-06-04（Fabrice Bellard，MIT），
  `https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz`。
- **为什么不用 2024-01-13**：那一版没有 `LRE_FLAG_UNICODE`，也没有
  `lre_get_alloc_count()`，`regex_tools.cpp` 直接编译不过；2026 版两者都有。
- `cutils.c` 必须一起编：`libregexp`/`libunicode` 调用的 `dbuf_put_*` 在那里。
  （quickjs-ng 把 DynBuf 内联进 `cutils.h` 且不带 `.c`，所以 xmake 里该文件
  是可选的，见 `fs_core` 的 `qjs_files`。）
- 许可：MIT，Copyright (c) 2017-2018 Fabrice Bellard（见各文件头）。
- 宿主回调（libregexp.h 末尾声明，非 TEST 编译需外部提供）由
  `src/tools/regex_tools.cpp` 实现：
  - `lre_check_stack_overflow` → 恒 false（单线程工具调用，栈充足）
  - `lre_check_timeout` → 恒 0（不设超时；工具级另有整体超时）
  - `lre_realloc` → `std::realloc`
- **头文件必须 `extern "C"` 包住**：上游头没有 C++ 链接保护（它本来就是
  给 QuickJS 内部用的），不包的话所有 `lre_*` 符号会被 C++ 改名，链接失败。
- 上游参考：QuickJS `quickjs.c` 的 `js_regexp_exec` 提供了 capture 数组用法
  （`lre_get_alloc_count` / `lre_get_capture_count`，capture 下标 0..count-1）。
- 升级路径：替换 `scripts/fetch_3rdparty.py` 里的 qjs 版本号并重跑脚本即可。

## webview 明细（备用 UI 宿主）

- 来源：`https://github.com/webview/webview`，**tag `0.12.0`**（stable release；该仓库
  没有 GitHub Releases，stable 以 tag 标记，最新即 0.12.0）。
- 文件：`core/include/webview.h`、`core/include/webview/webview.h`、
  `core/src/webview.cc`、`compatibility/mingw/include/EventToken.h`、`LICENSE`。
  目录布局：`3rdparty/webview/{include,src,compat-mingw}`。
- 许可：MIT（Serge Zaitsev / Steffen André Langnes）。
- Windows 后端还需要微软的 **WebView2 SDK 头文件**（`WebView2.h`、
  `WebView2EnvironmentOptions.h`），从 nuget 包 `Microsoft.Web.WebView2`
  的 `build/native/include/` 取，放 `3rdparty/mswebview2/include/`。
  运行期依赖系统里的 **WebView2 Runtime**（Win11 与较新的 Win10 自带）。
- 编译：`xmake f --webview=y` 会定义 `webview_host` 静态库（`WEBVIEW_STATIC`）；
  默认关闭，普通构建不受影响。MinGW 必须带 `compatibility/mingw` 头文件，
  否则缺 `EventToken.h`（Windows SDK 里 MSVC 才有）。
- 已验证：MinGW g++ 16.1 + `-DWEBVIEW_STATIC` 编译 `webview.cc` 通过。
- 定位：**备用路径**。主 UI 是 WhaleUI；webview 用于把现有浏览器版 UI 直接
  装进原生窗口（省掉本地 HTTP 服务 + 外开浏览器）。

## 本地 patch

- `dtl-1.20/dtl/Diff.hpp`：`bool trivial;` → `mutable bool trivial;`
  （GCC16 `-Wtemplate-body` 报错，见 notes `<3>`）；由 fetch 脚本自动打。
