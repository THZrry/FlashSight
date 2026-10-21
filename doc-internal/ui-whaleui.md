# WhaleUI 桌面 UI 接入（ui-whaleui）

> 状态：**代码已写完并接线**，用 `xmake f --variant=whaleui` 打开。
> 等 WhaleUI 完工（只剩拖放）后跑一遍本文 §4 的验收清单即可。
> 对应笔记：notes `<35>`、`<70>`。

## 1. 开关与构建

```sh
# 默认（浏览器 wasm + 本地 HTTP）
xmake f --variant=web -y --toolchain=mingw && xmake b

# WhaleUI 原生窗口
xmake f --variant=whaleui -y --toolchain=mingw \
        --whaleui_libdir=C:/aaa/WhaleUI/build/mingw/x86_64/release
xmake b
```

- 产物是 `flashsight-whaleui[.exe]`，和另外两个变体同目录共存。
- 变体宏 `FS_UI_WHALEUI` 只加在 `fs_ui_host`（`ui_dom_whale.cpp` +
  `ui_main_whale.cpp`）和 `flashsight`（`main.cpp`）上，`fs_ui` 本身不含
  变体宏——所以切变体只重编三个文件。
- `--variant=whaleui` 时不构建 `fs_ui_wasm`（不需要 wasm 包）。`ui_server.cpp`
  仍然编译（三个变体共用），资源头来自 `build/gen/`。
- 头文件：`3rdparty/whaleui/include/{whaleui.h,whaleui.hpp}`（WhaleUI 的
  include/ 副本；3rdparty 不入 git，重取方式见 `scripts/fetch_3rdparty.*`）。
- 依赖：WhaleUI 自己的 `libwhaleui-full.a` + SDL3/SDL3_ttf 预编译包 +
  xrepo 的 lexbor/utf8proc/SDL3_image。FlashSight **不重复 add_requires**
  （本机重装 lexbor 需要 VS 工具链），而是从 `--whaleui_pkgdir`（默认
  `%LOCALAPPDATA%/.xmake/packages`）直接找已装好的 `.a` 链接。
- `flashsight.exe` 的 after_build 会把 `SDL3.dll`/`SDL3_ttf.dll`/
  `dxcompiler.dll`/`dxil.dll` 和 `web/{style.css,icons/*}` 拷到 exe 旁边：
  UI 通过 VFS 按相对路径读样式，DLL 是运行期必需。

## 2. 分层：只换了最底下那一层

```
user (ui_app)  →  components / vdom (diff)  →  IDom  →  ┬ WasmDom（浏览器，现状）
                                                        ├ NullDom（单测）
                                                        └ WhaleDom（原生，本次新增）
```

`ui_app.cpp` / `ui_vdom.cpp` / `ui_components.cpp` **一行未改**。新增：

| 文件 | 作用 |
|---|---|
| `src/ui/ui_dom_whale.cpp` | WhaleDom：`whaleui_dom_*` 实现 IDom |
| `src/ui/ui_main_whale.cpp` | 桌面入口：建 App/Window、注入样式、挂 UiApp、跑事件循环 |
| `src/main.cpp` | 按 `FS_UI_WHALEUI` 二选一（HTTP 服务 / 原生窗口） |

## 3. 实现要点（WhaleUI 语义与浏览器不同处）

1. **Node 句柄是槽位下标，不是元素指针**。`Node::id` 索引 `slots_`。
   原因：WhaleUI 没有"改写这个文本节点的数据"的公开 API，所以
   `set_text` 只能**换一个 lexbor 文本节点**（插入新的 + 摘除旧的），
   而 diff 比较的是我们的 id，必须保持稳定。
2. **布尔属性按存在性处理**。`checked/disabled/selected` 的 `"0"` 走
   `remove_attribute`，`"1"` 走 `set_attribute(name,"")`。否则 `"0"` 也是
   "存在"，`:checked` 永远为真（复选框关不掉）。
3. **`value` 走 property 通道**。`whaleui_dom_set_value` 会分派到
   input 属性 / textarea 文本 / select 选中项——这是"发送后清空输入框"
   真正生效的前提。
4. **空字符串的 style = 删除该声明**。WhaleUI 的 `style_set` 会把
   `prop: ` 写进 style 属性，而 diff 用空值表示"这一轮不再设置"，
   所以空值改走 `style_remove_property`（终端抽屉收起时的高度就是这样)。
5. **文本节点替换而非原地写**（见 1）。
6. **remove() 摘除 + 销毁**。WhaleUI 的 `element_destroy` 只释放**已摘除**
   的节点；diff 调 remove 的对象正是它要丢弃的子树，不销毁就是每次重渲染
   泄漏（聊天记录会替换上千个节点）。
7. **键盘分两路**：
   - 终端键走**节点上的 keydown 监听**（`bind_key_input`）：终端需要修饰键
     （Ctrl+C 是控制字节不是字母 'c'），而 WhaleUI 的窗口回调只给 keycode。
     引擎只把 keydown 派发给它的 `focus_el`（鼠标按下时设置），所以**终端要
     先点一下才吃键**（见 §4 待办）。
   - 全局键（Esc）走**窗口回调**：Esc 必须"无论焦点在哪都生效"，而 DOM
     监听挂在元素上，焦点为空时收不到。窗口回调始终会触发。
8. **拖放**：`bind_drop_zone` 在目标上监听 dragover（preventDefault 才是
   接受拖放）与 drop。WhaleUI 的 OS 拖入带**真实路径**（浏览器只给文件名），
   所以目录直接按路径打开，文件按全路径交给模型的文件工具——比浏览器更强。
9. **外观**：palette 写成 `:root` 的 class、模式写成 `data-mode` 属性、
   字体写成 `--font-user/--mono-user/--font-size-user` 内联变量——与
   `web/style.css` 的变量体系一致（该文件的头注释就写着这套约定）。
10. **cell size**：WhaleUI 没有"测量元素文字宽度"的 API，用隐藏 `<canvas>`
    的 `measureText` + 元素 computed style 的 font 字符串测出列宽/行高，
    并用 ResizeObserver 跟随尺寸变化（一个 observer 只带一个回调，所以
    一次只观察一个终端）。

## 4. 打开开关后必须做的验收（按顺序）

1. `xmake -y` 通过，`flashsight.exe` 起窗口、界面有样式（不是裸 HTML）。
2. 顶部标签、主页三个入口、设置页双栏能点开切换；项目卡片能列出会话。
3. 输入框：**点进去**能打字、Enter 发送、发送后清空；Ctrl+Enter 同理。
4. 终端：新建终端 → **点一下终端区域** → 能敲键、能看到输出；
   Ctrl+C 能中断；终端内容能滚动。
5. 拖一个文件夹进窗口 → 打开为项目；拖一个文件 → 输入框插入附件路径。
6. 侧栏拖宽 / 终端抽屉拖高能改变尺寸，且切换标签后保持。
7. 深浅色跟随系统；设置里换配色/字体立刻生效。

## 5. 已知缺口（WhaleUI 侧未提供或与浏览器不同）

| # | 现象 | 原因 / 现状 | 需要 WhaleUI 提供 |
|---|---|---|---|
| ~~0~~ | ~~界面只画出 40px 顶部标签条，页面主体全空~~ | **已修**：WhaleUI `e87979c`（flex 主轴视口单位塌 1px）。WhaleUI 重新构建后，本文 §4 的验收 1/2/6/7 已通过：界面完整渲染、标签可切换、设置页双栏正常。复现程序与根因留在 WhaleUI `temp/incorrect1/`（15 个变体现已全部 ok） | — |
| **H** | **flex 项带 `overflow:hidden` 时塌成 1px**（探索页所有描述不可见） | WhaleUI `layout_flex` 把 `overflow` 非 visible 的项 min-content 地板降到 0，又把这个 0 当作 hypothetical main size（应为内容宽度，只作收缩下界），最后被 1px 地板兜住。与 `min-width` 无关、与样式表无关。复现：WhaleUI `temp/incorrect2/` 的 `overflow-span-flex`（1x342）vs `plain-span-flex`（228x18） | 修正 flex-basis:auto 的初始尺寸语义（`temp/incorrect2/README.md` §3） |
| **I** | **`<button>` 的显式 `height` 被 UA `min-height` 压掉**（开关 knob 脱轨、按钮偏高） | WhaleUI UA 主题给 `button` 设了 `min-height: var(--btn-h)`（fluent 32px，`theme_css.cpp:179`），而 `min-height` 恒优先于 `height`；浏览器 UA 没有这条。复现：`button-explicit-size` 34x32 vs `div-explicit-size` 34x19 | 去掉 `min-height` 或仅在无显式高度时生效（§4） |
| **J** | **grid `auto-fill minmax(400px,1fr)` 列宽算成 122px**（设置页挤成一列、label 折行、toggle 8px） | `layout_grid` 的 `auto-fill` 未把 `minmax` 下限计入列数，`1fr` 也没分剩余空间，列宽退化成内容宽度。复现：`settings-grid` field 122x64（应 ~510），对照 `settings-block` 正常 | 修 `repeat(auto-fill, minmax(...))`（§5） |
| **K** | flex 行含 `select` 时可用宽度少 52px | 同一 DOM 换成 div 就正确（918 vs 866），嫌疑是控件 UA 外边距在 flex 分配里重复计入 | 见 `temp/incorrect2/README.md` §6 |
| A | 程序化 focus 后按键不生效（终端新建后要先点一下） | `whaleui_dom_focus` 只写 activeElement 镜像；引擎的 `focus_el`（决定 keydown 派发和 `:focus` 样式）只在鼠标按下时设置 | 一个"设置引擎焦点"的 API（或在 `whaleui_dom_focus` 里同步） |
| B | 终端滚动跟随不工作 | 没有读滚动位置的 API（scrollTop/scrollHeight/clientHeight） | 元素滚动读数 + `scrollTo` |
| C | 文件上传按钮点了没反应 | 需要原生文件选择器；`folder_picker` 目前只有选目录 | FsApi 增加"选文件"入口（或在适配层用 Win32 对话框） |
| D | 图标（Font Awesome）可能显示为方框 | WhaleUI 无 `@font-face`；字体族名取自**文件名**（`fa-solid-900`），而 CSS 写的是 `Font Awesome 6 Free` | `@font-face` 支持，或在 shell HTML 里加一段 `.fa{font-family:"fa-solid-900"}` 覆盖 |
| E | 项目卡片宽/窄布局不切换 | `web/style.css` 用了 3 处 `@container`，WhaleUI 未实现容器查询 | `@container` 支持（或把这几处换成 `@media`，会同时影响浏览器版，需权衡） |
| F | 终端概览的 `ch` 单位定位可能不准 | `ch` 单位在 WhaleUI 的 `len_px` 里没有专门处理（`max-content` 有） | 确认/补齐 `ch` 单位 |
| G | 自定义标题栏（无边框 + 最大化/最小化）未接 | 目前用系统边框；无边框需要 HitTest + 拖拽区 | 已有 `set_hit_test`，接入即可 |

以上 A/B/C/E 会在打开开关后第一次运行时暴露，属"接入后要修"的清单；
`doc-internal/ui-events.md` 的界面规划不受影响。

**H/I/J/K 是 2026-10-17 首次在真实页面上跑出来的新缺口**，复现程序与根因在
WhaleUI `temp/incorrect2/`（`incorrect2.cpp` + `README.md`）。它们不影响
浏览器/wasm 路径，只影响 WhaleUI 桌面后端；当前界面能正常渲染和交互，
但这几处视觉不对。

### 缺口 0 的临时规避（仅在 WhaleUI 修复前使用，勿改应用代码）

应用根容器是两层 `height:100vh` 的 flex column，触发上面那条缺陷。实测可行的规避：
把**内层**容器从 `height:100vh` 改成 `min-height:100vh` 或 `height:100%`
（外层保持 100vh），或让外层容器不是 flex 容器。三种都只影响 WhaleUI 后端，
浏览器/wasm 路径不受影响。当前决定是**先记录、等上游修复**，不改应用代码。
（2026-10-17 上游已修，本节仅留作记录。）

## 6. 回退

`xmake f --variant=web` 即回到浏览器/HTTP 路径。三个变体互相独立：变体宏只
加在 `fs_ui_host`（两个后端文件）和 `flashsight`（`main.cpp`）上，所以切换
只重编三个文件，不会互相污染。
