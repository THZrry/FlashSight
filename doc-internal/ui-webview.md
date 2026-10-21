# Web / Webview / WhaleUI 三个版本

> 状态：**已实现并编译通过**（2026-10-20）。Web 与 Webview 已进 CI；WhaleUI 仍只在本地构建。

## 1. 三个构建变体

UI 层（`ui_app.cpp` / `ui_vdom.cpp` / `ui_components.cpp`）完全共享，只有最底层的
`IDom` 实现不同：

| 变体 | 命令 | DOM 后端 | 窗口 |
|---|---|---|---|
| **Web**（默认） | `xmake f --variant=web` | `WasmDom`（emscripten/val） | 用户浏览器标签页 |
| **Webview** | `xmake f --variant=webview` | `WebViewDom`（webview + 注入 JS） | 原生窗口（WebView2 / WebKitGTK / WKWebView） |
| **WhaleUI** | `xmake f --variant=whaleui` | `WhaleDom`（WhaleUI C DOM） | WhaleUI 自绘窗口 |

三个变体共用一个输出目录，**可执行文件名带变体**
（`flashsight-web` / `flashsight-webview` / `flashsight-whaleui`），所以先编
一个再编另一个不会互相覆盖。`--variant` 是单值选项，不可能出现"两个开关同时
为真"的矛盾（以前 `--whaleui=y` 不会清掉 `--webview`，于是选了 WhaleUI 却
出来 Web 包）。

变体宏只加在 `fs_ui_host`（两个后端文件）和 `flashsight`（`main.cpp`）上，
`fs_ui` 本身没有变体宏，所以切变体只重编三个文件、再链接。

**显示方式由编译期决定**：Web 版就是浏览器，Webview / WhaleUI 版就是原生窗口，
启动时必定按这个方式显示，没有运行时开关去换一个。

**三者是并列的访问方式，不是叠加的**：只有 **Web 版**启动内嵌 HTTP 服务（`ui_server.cpp`），
因为那里服务就是 UI。Webview / WhaleUI 版有原生窗口，**不启动服务** —— 起一个没人
需要的服务器只会造成"两个界面、两个端口"的困惑：

```
Web 版:      UI server: http://127.0.0.1:18925/        (默认，仅本机)
             UI server: http://127.0.0.1:18925/ (password required remotely)
Webview 版:  UI: webview (native window)               (无服务)
WhaleUI 版:  UI: WhaleUI (native window)               (无服务)
```

## 1.1 远程访问（设置-权限-Web 服务，仅 Web 版）

| 设置键 | 默认 | 含义 |
|---|---|---|
| `server.host` | `127.0.0.1` | 监听地址。`0.0.0.0` = 局域网/远程可达 |
| `server.port` | `0` | 0 = 系统分配空闲端口；>0 = 固定端口 |
| `server.password` | `""` | 非空时远程连接需要密码 |

改动**实时生效**：`settings.set` 收到这三个键就重新绑定监听器，不用重启进程。
实现要点（都是踩过的坑）：

- **端口默认保持不变**：`server.port = 0` 表示"启动时自动选"，不是"每次重绑都重新
  随机"。以前只改密码或监听地址也会换到一个全新端口，因为重绑走了 `port = 0` 那条
  路。现在只有用户明确写了不同的端口才换。
- httplib 的 `Server` 不能在 listen 之后换绑定地址，只能换一个新实例。
- **先绑定新端口，再关旧监听**：反过来的顺序会留下一段"没有任何监听"的窗口，那期间
  的请求（包括页面自己的轮询）都会失败。先绑成功也顺带证明了新端口确实可用；绑不上
  就保留原监听并把错误显示在设置页。
- **重启不能在被服务的那条请求线程上做**：`stop()` 要 join 监听线程，而监听线程
  退出时又会 join 线程池，当前请求本身就在池里 → 死锁（浏览器看到的是请求挂住）。
- **但也不能每次调用都起一个 detached 线程**（这是曾经的闪退根因）：连续两次设置
  （先切地址、再改端口）会让两个 helper 同时进 `stop()`；第二次 `join()` 一个已经
  join 过的 `std::thread` 会抛 `std::system_error`，detached 线程里无人捕获 →
  `std::terminate`，进程无任何输出直接消失。现在只有**一个常驻 rebind worker**：
  `restart()` 把最新目标写进槽位并 `notify`，worker 串行执行并合并突发请求（表单
  语义：最后一次生效）。worker 在 `~UiServer` 里置 quit 并 join。
- **`stop()` 必须先关掉正在进行的 SSE**：页面常挂一条 `/api/stream`，它的 chunked
  处理器要一直跑（等下一个事件）才不会结束；它占着池里一个 worker，`stop()` 的
  join 就永远等不到它 → 重绑卡死、旧监听不释放。**停止标志必须按"代"隔离**：每次
  `start()` / 重绑都新建一个 `shared_ptr<atomic<bool>>`，SSE 处理器捕获自己那一代
  的 flag。用一个共享标志时，退役旧监听会顺带杀掉新监听的 SSE。keep-alive 等待
  0.5s，保证 `stop()` 最坏只等半秒。**这是"改端口没反应 / 0.0.0.0 没有服务"的最后
  一层根因**——HTTP 层不挂 SSE 单独测时重绑全都正常。
- **新监听先 accept，再退役旧监听**：换入新实例 → 起 `listen_after_bind` → 等
  `is_running()` → 才给旧实例置 stop 并 join。反过来的顺序会留下空窗，旧页面探测
  新端口失败就停在死地址上（"后面什么操作都做不了"）。端口变化时旧 socket 再多活
  1.2s，给页面完成探测 + `location.replace`。
- **listen 线程要按值捕获 `httplib::Server*`**，不能 `[this]{ impl_->svr->... }`：
  线程真正运行时读的是**当前**的 `impl_->svr`，重绑换过实例后就会去 listen 新实例，
  而它已经有自己的 listen 线程了。
- **`--port` 要写回 `server.port`**：命令行端口原来只影响本次 `server.start()`，
  配置里还是旧值；之后任意一次改地址都会 `apply_server_settings()` 读配置，把端口
  悄悄搬回旧值——这就是"本地的也变了"。`apply_server_settings()` 在服务未运行时
  直接返回，避免启动前误触发重绑。
- `stop()` 会摘掉 FsApi 的事件监听器，重启前要重新注册，否则 UI 再也收不到事件。
- 所有路由 + 鉴权门的注册抽到 `configure(httplib::Server&)`，重绑时对新实例重放一遍。
- **端口/密码输入框在 Enter 或失焦时提交**，不是每敲一个字符。按字符提交会逐位重绑
  （"1" → "18" → "189"…），每次重绑都关掉还在传下一个按键的那条连接，最后一次可能
  落在没人指定的端口上——这正是"改端口好像没生效"的原因。失焦也提交是因为多数人改完
  就点别处。**非法值（非数字 / 超出 0-65535）直接拒绝并 toast**，绝不静默钳位：以前
  `p > 65535 → 65535` 会把一个笔误变成端口 65535 存进配置。
- 监听 `0.0.0.0` 时设置页显示**真实局域网 IP**（`UiServer::lan_ip()`：开一个未连接的
  UDP socket connect 到 8.8.8.8，再 `getsockname` 读本端地址；跨平台、自动跳过
  down/虚拟网卡），而不是没法照抄的 `<本机 IP>` 占位符。
- **换端口时页面自动跳转**：重绑前通过 SSE 推 `server.moved`（只带**端口**，URL 由
  页面用自己的 `location.hostname` 拼——发绝对地址会把远程客户端送到它自己的
  127.0.0.1）。页面收到后先用 `no-cors` 请求探测新端口，**通了才 `location.replace`**；
  立即跳会落在浏览器错误页（那时新监听还没起来）。探测约 6s 仍失败才显示"服务已更换
  端口"卡片。SSE 彻底断开（readyState=2）时也会提示，防止页面无声死掉。

**鉴权**：只约束远程来源。回环（`127.0.0.1` / `::1`）始终免密。

- `GET /api/auth` → `{required, authed}`：页面挂载前先问一次。
- `POST /api/login {password}` → `{token}`；密码错回 401。
- 之后所有 `/api/*` 请求带 `Authorization: Bearer <token>`。
- **页面外壳本身是公开的**（`/`、`style.css`、`fs_ui.js`/`.wasm`）：它不含任何数据，
  而且必须先加载出来才能显示密码框。数据全在 `/api/*` 下，那部分仍然受保护。
- 令牌只存内存，进程重启即失效（密码可能刚改过，旧的远程会话不该继续有效）。

**密码遮罩**是 `web/index.html` 里的纯 HTML/CSS/JS，不是 C++ UI 的一部分：它必须在
wasm 挂载**之前**就能拦住页面。`startApp()` 是唯一的启动入口——模块可能在遮罩期间
就加载完了，那时 `onRuntimeInitialized` 会跳过启动，密码通过后必须由 `startApp()`
补上，否则登录成功但界面空白。

## 2. 静态资源内嵌

三个变体都内嵌 `web/`（xmake 的 `pack_web_assets` 在 `fs_ui` 的 `before_build`
里生成 `src/ui/ui_assets_gen.hpp`）：

- **Web**：`ui_server.cpp` 从内嵌表里按 URL 路径取字节（含 `fs_ui.js` / `fs_ui.wasm`）。
- **Webview**：`ui_main_webview.cpp` 把 `style.css` + 两个图标 CSS 内联进
  `<style>`，字体 `fa-solid-900.woff2` 转成 `data:font/woff2;base64,...` 填进
  `@font-face`（因为 `webview_set_html` 用的是 `NavigateToString`，文档没有 base URL，
  相对路径无法解析）。
- **WhaleUI**：`ui_main_whale.cpp` 通过 WhaleUI 的 VFS 按相对路径读 `web/`，
  所以构建后会把 `web/` 拷到 exe 旁边。

**注意**：`pack_web_assets` 的**函数体必须内联在 `before_build` 回调里**。
xmake 只在 target hook 自己的函数体内暴露 `io`/`os`/`path`；调用脚本级的函数
会报 `attempt to call a nil value` 或 `attempt to index a nil value (local 'io_lib')`。
这个坑在改这段代码时很容易再踩一次。

## 3. WebViewDom 适配器

`src/ui/ui_dom_webview.cpp`，`#ifdef FS_UI_WEBVIEW`。

### 为什么不能复用 WasmDom

`WasmDom` 走 `emscripten/val.h`，只在 emscripten 模块内存在。桌面进程要跨引擎边界，
webview 只给两个原语：`webview_eval(w, js)`（无返回值）和
`webview_bind(w, name, fn, ud)`（把 C 回调暴露成 JS 函数）。

这决定了整个适配器的形态：

1. **节点 id 由 C++ 分配**。JS 侧把节点存进以该 id 为下标的表，所以没有任何调用
   需要返回值。
2. **读操作靠镜像**。`get_value()` / `is_focused()` / `scroll_top()` 必须立即返回，
   所以适配器维护一份镜像：我们自己写入时更新，用户改动时由 JS 通过
   `fs_ui_push` 回调更新（在事件处理器之前送达，所以处理器里读到的已是新值）。

### 通信格式

单向调用统一走 `globalThis.__fsDomCall(op, i, j, a, b)`（一个 switch）。
回调方向每个用途一个绑定（`fs_ui_fire` / `fs_ui_push` / `fs_ui_split` / ...）。

**关键**：webview 的绑定是**位置参数**的 JS 函数（内部
`params = [name].concat(arguments)`），所以 C 侧收到的是 JSON **数组**，
形如 `[hid, "value"]`。不能把数组当作单个参数传。

### 踩过的坑

| 现象 | 原因 |
|---|---|
| 窗口全黑，什么都不显示 | `webview_init()`（注册文档创建脚本）必须在**第一次 `set_html` 之前**调用，否则 glue 不会进入当前文档 |
| 页面能渲染但点击无反应 | 用 PowerShell 模拟点击时坐标算错（窗口实际 1600x1025，脚本按 1294 算）。程序内 `webview_eval` 触发 `.click()` 是正常的，可用 `FS_UI_WEBVIEW_DEBUG=1` 验证 |
| 点击后整页变空 | `insert_before` 没把 `ref` 传给 JS，导致 diff 重排时全部追加到末尾。现在 ref 走 `a` 槽位 |

### 诊断

`FS_UI_WEBVIEW_DEBUG=1` 会：
- 探测 glue 是否就位（`typeof __fsDomCall`）；
- 打印节点数/监听器数/标题按钮数；
- 程序化点击第一个标题按钮并回报结果；
- 之后每次事件都打印 `hid` 与 value。

## 4. webview 依赖

见 `doc-internal/thirdparty.md`：`3rdparty/webview`（0.12.0，MIT）+
`3rdparty/mswebview2`（WebView2 SDK 头文件）。

平台差异（`xmake.lua` 的 `webview_host` target）：

| 平台 | 编译期 | 运行期 |
|---|---|---|
| Windows | `3rdparty/mswebview2/include` + `compat-mingw`（补 `EventToken.h`），链 `shlwapi`（内置 loader 用 `PathFindFileNameW`） | WebView2 Runtime |
| macOS | `add_frameworks("WebKit")` + `dl`（`webview.cc` 是纯 C++ 走 `objc_msgSend`，不需要 .mm） | 系统自带 |
| Linux | `find_package("pkgconfig::gtk+-3.0")` + `pkgconfig::webkit2gtk-4.1` | `libgtk-3-0` + `libwebkit2gtk-4.1-0` |

pkg-config 的结果按 `links` / `linkdirs` / `includedirs` 结构化传给 xmake，不要手工
split 输出（会丢链接顺序，且路径含空格时会断）。

## 5. 发布包

`scripts/package.py` 出包，命名 `flashsight-<version>-<platform>-<variant>.zip|tar.gz`
（Windows 用 zip，其他用 tar.gz 以保留可执行位）。CI 见 `.github/workflows/build.yml`。

Windows 上按 PE 导入表决定要带哪些运行时 DLL：MinGW 链的是 `libstdc++-6.dll` +
`libgcc_s_seh-1.dll`，llvm-mingw 链的是 `libc++.dll` + `libunwind.dll`，写死一份
列表一定会漏。

## 6. 已知限制

- **WebView2 Runtime** 是运行期依赖（Win11 / 较新 Win10 自带）。
- **文件选择器**：`bind_file_picker` 目前只把文件名回传（`{"names":[...]}`），
  没有实现真正的字节上传。桌面版需要后续用平台对话框 + 直接读文件。
- **镜像读取**的时序边界：`get_value()` 在用户输入后、`input` 事件处理器内是准确的；
  但在 DOM 被脚本外部修改（本适配器之外）时不会同步。
- **桌面版没有远程访问**：Webview / WhaleUI 版不启动服务，所以它们不能被远程连接。
  这是"三种方式并列"的直接后果——需要远程就用 Web 版。
- **远程是明文 HTTP**：密码和令牌都不加密。局域网/内网可用，公网请走隧道
  （SSH 端口转发 / Tailscale 之类），不要在公网直接暴露端口。
- **密码设置向导**还没做；遮罩层已经就位，将来加向导（首次启动配置模型等）复用同一层。
