# FlashSight Notes

The process of project: P0 环境搭建 + P1 AppCore

<1>(SOLVED): 网络不可直连 GitHub，本机 v2ray 代理 127.0.0.1:10809（系统代理已启用）。
  所有 curl/git clone 需 `-x http://127.0.0.1:10809`。curl 直连 exit 6/28，走代理后正常。
  经验：环境里 Bash 是 cmd 兼容（无 unix 管道工具），脚本一律 .bat；且每次 Bash 调用是全新进程，
  set 的环境变量不保留，必须写进 .bat 里或每条命令带上。

<2>(SOLVED): mbedtls 4.x development 分支不可用：`build_info.h` include `tf-psa-crypto/build_info.h`，
  且 3rdparty 目录里没有对应子模块。改定稿 3.6.4 LTS（v3.6.4 tag），并需 `git clone` mbedtls-framework
  到 `framework/`（release 包不带该子模块）。CMake 配置时 config.py 依赖 framework 内 mbedtls_framework 模块。
  构建还要 `-DCMAKE_C_FLAGS=-Wno-error=format`（GCC16 对 `%I64d`/long long 的 -Wformat 升为 error），
  链接要 `-lcrypt32 -lbcrypt -lws2_32`。这些已全部验证，写进 temp/environment.md 和 scripts/fetch_3rdparty.bat。

<3>(SOLVED): dtl v1.20 `enableTrivial() const` 内给成员 `trivial` 赋值，GCC16 报 -Wtemplate-body。
  解法：`bool trivial;` → `mutable bool trivial;`（Diff.hpp 第 66 行附近）。已 patch 到 3rdparty 里。

<4>(SOLVED): cpp-httplib(master) 支持 mbedTLS（CPPHTTPLIB_MBEDTLS_SUPPORT），
  与 mbedtls 3.6.4 链接 OK，并实测通过 127.0.0.1:10809 代理请求 raw.githubusercontent.com 返回 200（1.4MB）。
  注意 httplib 不读系统代理环境变量，代理需在应用层 set_proxy（P2/P3 网络工具时注意）。

<5>(SOLVED): xmake 在 Windows 上默认找 VS 工具链，本机没有 VS。配置时 `xmake f -y --toolchain=mingw`，
  之后 `xmake -r` 干净构建。空目录 add_files 的 warning 用 `os.files()` 过滤掉。

<6>(SOLVED): repo 曾误提交 3rdparty_tmp（mbedtls 4.x 解压残留）与诊断 scratch 文件，
  已 git rm --cached 清理并补干净提交。3rdparty/ 不入 git（rules 要求），fetch 脚本可重放。

<7>(SOLVED): P1 AppCore 存储层+核心管理器+FsApi 骨架+M1 单测全过（11 用例 121 断言）。
  - ghc::filesystem 不暴露 `error_code`（用 `std::error_code`），全仓替换过；
  - ifstream/ofstream 不接受 ghc::path 直接构造（用 `.string()`）；
  - Windows 文本模式 CRLF 翻译坑：写入文件与 patch 记录必须统一二进制模式（测试用 std::ios::binary 写盘），
    否则回滚 `cur==after` 比较失败报假冲突——P3 工具层写文件也须遵守；
  - SettingsStore 项目级按 pid 懒构建缓存（FsApi::store_for），避免每次重建；
  - 权限升级（低→高）令牌确认流：后端发令牌，UI 确认后 set_permission_confirm 生效；
  - 回滚语义：删除该 turn 及之后 turns（keep=idx），patch 逆向还原，冲突如实报错不静默；
  - 分叉：复制 full.json 前缀到新 sid，snapshots 按保留 turns 裁剪；
  - 遗忘：full.json 标 forgotten 不物理删。

## 当前状态

- P0+P1 完成，git log e73f384。roadmap 10-appcore.md 已勾选（剩远程入口、spawn/kill/poll 在 P3）。
- 下一步：P2 Provider（src/provider，见 roadmap/20-provider.md）。
<8>(SOLVED): P2 Provider 层完成（FsApi provider.* 真实现，FakeNet 单测不碰真网）。
  - NetClient 真实现（httplib+mbedTLS）：SSLClient 与 Client 无公共基类，用模板 execute() 分发；
    httplib::Headers 不支持 operator[]，用 emplace；Error 枚举是 Success/Timeout/SSLConnection 等。
  - mbedtls 链接：xmake 的 add_links 顺序在 deps 之前导致未解析符号；
    解决：add_ldflags 传完整 .a 路径（追加在链接命令末尾），系统库 -lbcrypt -lcrypt32 也放末尾。
  - ProviderStore：providers.json 读写（600 权限），to_json 不回显 apiKey（防泄漏），save 时补回。
  - ProviderClient：OpenAI Chat/Response/Anthropic 三形态，请求构造+响应解析（usage/tool_calls），
    远程错误原样透出（provider.md §4 不兜底）。
  - ProviderRuntime：用量记录（成功失败都记 JSONL）+ 重试（指数退避）+ 连续失败计数（refreshConfig 清）。
  - ModelTable：四级回退（用户>API>预置>保守缺省），/models 一键获取失败回退提示不清空。
  - 预置模型表 builtin_models_preset：gpt-4o/o4-mini/claude 系列等。

## 当前状态（更新）

- P0+P1+P2 完成。roadmap 20-provider.md 已勾选（剩钥匙串 TODO）。
- 下一步：P3 Agent 引擎（src/agent，见 roadmap/30-agent.md）。

<8>(SOLVED): P2 Provider 层完成（FsApi provider.* 真实现，FakeNet 单测不碰真网，19 用例 169 断言全过）。
  - NetClient 真实现（httplib+mbedTLS）：SSLClient 与 Client 无公共基类，用模板 execute() 分发；
    httplib::Headers 不支持 operator[]，改 emplace；Error 枚举是 Success/Timeout/SSLConnection 等。
  - FakeNet 测试桩：responses 弹出队列 + requests 记录，mutable 兼容 const 接口（const_cast 换 mutable）。
  - mbedtls 链接：add_links 顺序在 deps 之前导致未解析符号；用 add_ldflags 传完整 .a 路径
    （追加在链接命令末尾）+ 系统库 -lbcrypt -lcrypt32 也在末尾；xmake 无 os.absolute，用 projectdir 拼接。
  - ProviderStore：providers.json 读写（600 权限），to_json 不回显 apiKey（脱敏），save 时补回。
  - ProviderClient：OpenAI Chat / Response / Anthropic 三形态，请求构造 + 响应解析（usage/tool_calls），
    远程错误原样透出（provider.md §4，不兜底）。
  - ProviderRuntime：用量 JSONL 记录（成功+失败都记）+ 重试（指数退避）+ 连续失败计数（refreshConfig 清零）。
  - ModelTable：四级回退（用户自定义 > API 获取 > 预置 > 保守缺省），/models 一键获取失败回退并提示。
## 当前状态（更新）

- P0+P1+P2 完成。roadmap 20-provider.md 已勾选（剩钥匙串+套餐 TODO）。
- 下一步：P3 Agent 引擎（src/agent，见 roadmap/30-agent.md）。

<9>(TIPS): rules/addon1.md 补充了 P3 关键设计，必须落实：
  - 预置子代理：/local-research（只读查 bug）、/complete-step（分派完成 step，不支持并行）、
    /return（子代理多轮后输出结束自动执行，只回主代理摘要）、/round-plan（本轮规划，
    阻塞不并行，直接主会话显示）、/check-files（指定文件分析改错返回摘要）。名字可对接行业惯例。
  - 提示词必须显式建议模型用子代理解决问题。
  - 子代理历史不进主会话历史但必须保留（会话存储需相关设计）。
  - 默认子代理嵌套 max 3 层，设置可改。
  - 引导机制：本轮结束传递（显式表达）与当前传递（工具调用完成后输入，不需要显式）；
    auto 模式也有队列（每轮可加备注）；"引导当前"入当前队列；当前队列 >1 合并输入；
    可编辑/拖动排序/删除。这影响 Loop 的输入处理。
  - 界面：滚动优化（底部时自动滚动否则不滚动）、会话进入滚动同步、
    侧边栏项目+会话罗列（无打开标签页概念）、排序/筛选/搜索/折叠、标签页缓存秒开。

<10>(SOLVED): P3 Agent 引擎完成（34 用例 288 断言全过，M3 验收达成）。
  - AgentLoop 状态机：idle/planning/toolCalling/waiting/turnDone；send 运行中进引导队列；
    高风险工具（写/删/网/终端）手动+自动都走 permission.prompt；approve 拒绝回填继续。
  - 工具调用格式兼容：ProviderClient 返回 OpenAI 原生 {function:{name,arguments}}，
    Loop.handle_tool_call 兼容扁平与原生两种格式（arguments 字符串解析为 Json）。
  - AgentHost 宿主：按 sid 管理 Loop；工具回调接线（terminal/net/vision 走宿主回调，
    未接线返回 not-wired）；call_model 无 provider 时回显模式（单测可跑通）。
  - HistoryBuilder：短视图 + token 预算 + 压缩折叠 + forgotten 剔除；
    压缩条件 msgs.size()>1（修过 >2 导致小预算不压缩的 bug）。
  - 引导队列（addon1.md）：immediate/round-end 双队列，合并/编辑/排序/删除/to_json。
  - 子代理：spawn/深度上限/并行上限/cancel/timeout 落库；预置 7 代理
    （addon1.md 5 个：local-research 只读、complete-step 不并行、return 自动摘要、
    round-plan 阻塞、check-files + research/explore）。
  - 文件工具：read 分页 + 二进制守卫 + 路径逃逸检查；diffWrite 精确匹配一次；
    Windows 文本模式 CRLF 坑延续（工具内部统一二进制读写）。
  - 终端/网络/vision/MCP 全部回调式（宿主层实现，工具层只调回调）。
  - 写入干扰教训：本轮输出流频繁注入垃圾 token；全部经"temp 写->扫描->复制->构建验证"
    可靠路径，源码全仓扫描干净。
## 当前状态（更新）

- P0+P1+P2+P3 完成。roadmap 30-agent.md 已勾选（剩 network.search 调研）。
- 下一步：P4 UI（等 WhaleUI；fs_ui 空壳+事件类型先行）或收尾提交。

<11>(SOLVED): P0-P3 回顾审计出的缺口（用户 2026-10-11 提出复查，已全部落地）：
  1. 设置面后端键缺失：模型偏好（model.pref.*）、子代理开关/模型持久化
     （subagents.<name>.enabled/model）、长期记忆（存储+读写 API）、外观主题/字体键、
     浏览器/电脑控制开关（browser.*/computer.*）、快捷键表（shortcuts.*）。
  2. 环境变量合并注入逻辑缺（terminal.md 5：全局+项目合并，spawn 时注入）——只有键，无函数。
  3. Anthropic 会话调不通工具：HistoryBuilder 只产 OpenAI 格式 tool_calls，
     Anthropic 的 tool_use/content-blocks 装配缺失。
  4. x-opencode-session 套餐协议未落地（等调研子代理结果）。
  5. 单会话持久记忆缺：需新增 session.summarize 工具（每轮记要点到 turn 要点字段），
     HistoryBuilder 压缩时把要点附加到上下文（平衡调用与记忆，支持几百轮）。

<12>(SOLVED): 缺口补齐落地细节（39 用例 340 断言全过）：
  1. 设置键：model.pref.*/subagents.*/model.longMemory/ui.theme-font/ui.* 全部键+默认值；
     merged_env()/merged_cli_tools()（全局+项目覆盖，ignoreGlobal 只取项目）。
  2. Anthropic 装配：HistoryBuilder.build_anthropic（system 单独字段 + text/tool_use/tool_result
     blocks；兼容扁平与 OpenAI 原生 toolCalls；tool 结果经 turn.toolResults 回填）。
  3. 持久记忆：session.summarize 工具（points 数组 -> session.keypoint 事件）；
     AgentHost.emit 闭包自动注入 sid；pending_keypoints_ 在 persist_turn 写入 turn.keyPoints；
     build/build_anthropic 压缩时收集被裁 turns 要点（最多 20 条）附加到折叠条。
  4. x-opencode-session（子代理实证 opencode 仓库）：亲和/路由 id 非 token；
     kPlan 自动带 x-opencode-session(+id)/x-opencode-client；解析 .cost（"0"->quota，
     金额串->money）；402（余额零）/429（限流）映射为带语义的 kRemote 错误。
  5. ChatRequest 加 session_id 字段（亲和 id 透传）。
## 当前状态（更新）

- P0-P3 + 缺口补齐全部完成。UI 以外后端全通。
- 下一步：P4 UI（等 WhaleUI；fs_ui 空壳+事件类型先行）或收尾。

<13>(SOLVED): UI 原型 + C++ 响应式框架 + 兼容层 + wasm 分支桩（用户 2026-10-11 指示）。
  - 设计稿分析：temp/design.json 只有 5 种控件（tabs/box/text/button/listItem），
    4 个 frame（主页项目/用量/设置/会话），frames 仅几何，控件全在 groups.items。
  - web/ 纯 HTML+CSS 桌面风原型（非 Material）：标题栏/tabstrip/主页双栏/会话
    （顶栏+规划栏+历史+侧栏+输入+终端）/设置双栏/用量图表占位；tab 用 :target 纯 CSS 切换，无 JS。
  - src/ui 兼容层 ui_dom.hpp：IDom 抽象（create/append/remove/clear/text/attr/visible/on）；
    NullDom 空实现（含调用记录，单测用）；BrowserDom（wasm，经 web/glue.js）。
    WhaleUI 就绪后加 WhaleDom 实现即可，UI 逻辑不动——不预设 WhaleUI API。
  - 响应式 ui_reactive.hpp：Signal<T>（shared_ptr+RAII 退订）/Effect.watch
    （pack 展开重订阅）/bind_text/bind_visible。修过两个真 bug：
    notify 时遍历 map 又 erase 导致访问违例（改快照后调用）；dispose 未退订
    （先 clear subs 再 reset）。
  - UiApp 四页骨架（class 名与原型一致，方便对照），Signal 驱动切页/显隐。
  - xmake fs_ui_wasm target（option(wasm) 默认关；仅 src/ui+common 进 wasm；
    fs_core 进程/网络暂不进）。本机无 llvm/emscripten，只写不编。
  - 单测 tests/test_ui.cpp 5 用例；全量 44 用例 361 断言通过。
## 当前状态（更新）

- P0-P3 + 缺口补齐 + UI 原型/框架/兼容层/wasm 桩全部完成。
- 等：WhaleUI 纯 C API（接 WhaleDom 实现）、llvm/emscripten（编 wasm 验证）。

<14>(SOLVED): Widget 库扩充（用户要求控件多些 + 前端框架思路 + 桌面进程内直调）。
  - ui_widgets.hpp/cpp：Widget 基类 + 21 种控件：展示（Text/CodeBlock/Badge/
    Progress/Spinner/EmptyState）、输入（TextInput/TextArea/SearchBox）、
    选择（Button/Select/Checkbox/RadioGroup/Toggle）、容器导航（Tabs/Accordion/
    ListView/Modal/Tooltip/Splitter）。全部经 IDom 渲染，浏览器与桌面同一套逻辑。
  - ui_backend.hpp：IUiBackend（call/subscribe）+ NullBackend（单测/演示）。
    桌面原生预留 NativeBackend 直接调 FsApi（进程内函数调用，不走 HTTP）；
    以后远端再加 HttpBackend。
  - UiApp 用 widgets 重写：会话页（顶栏 Select/Toggle/挂起Button/刷新、
    规划 Accordion、消息 ListView、右侧 Tabs、TextArea+发送Button、ask Modal、
    终端显隐绑定）、设置页（Select/TextInput/Checkbox/测试连接Button）、
    用量页 Progress 水位；AppState 扩展 9 个信号；Widget 存活期由 own<> 管理。
  - web/style.css 补 widget 样式（badge/progress/modal/toggle 等）。
  - tests/test_ui2.cpp 10 用例；全量 54 用例 390 断言通过。
## 当前状态（更新）

- UI 层：原型 + 兼容层 + 响应式 + 21 控件 + 后端抽象 + wasm 桩全部完成。
- 等：emscripten（用户准备中，到时编 wasm 验证）、WhaleUI 纯 C API（接 WhaleDom）。

<17>(SOLVED): addon2 后端增强全部落地（68 用例 516 断言全过；UI 零改动）。
  - A 工具：A1 工具真执行（execute_tool + run_loop 循环回填，修了原占位 "(tool executed by host)"）；
    A2 dir.check；A3 file.regex（QuickJS 引擎移植，见 <15>）；A4 权限门（full 才可项目外）；
    A5 噪声检测（罕见 script 短串+引号佐证）；A6 file.ls 基础/详细双模式。
  - B 健壮性：B1 中断-继续（saved_messages_ 断点 + session.continue；子代理 steer）；
    B2 进程 todo（TodoStore，owner=sid/子代理 id，独立进程树）；B3 计划评审 revise（评价注入引导）。
  - C 工作流：WorkflowStore（步骤状态机 + graph_json 给侧栏图/嵌套列表）；
    workflow_start/retry/skip/steer；子代理完成经 subagent.done 事件推进步。
  - D 环境：lsp 工具 + ctx.lsp_call 回调 + lsp.servers 键；browser-control skill + browser_load 加载口。
  - skill：SkillRegistry 中英双语，model.thinkingLang（auto/zh/en）切换；工具 description 全双语。
  - 关键坑：libregexp capture 数组下标为 0..capture_count-1（原来 <= 越界导致
    basic_string::_M_create 崩溃）；lre_exec 返回 1=匹配/0=不匹配/<0=错误。
  - 3rdparty/qjs-regex 不入 git；scripts/fetch_3rdparty.* 已补拉取步骤；
    doc-internal/thirdparty.md 记出处/许可；doc-internal/addon2-backend.md 记实现。

## 当前状态（更新 2026-10-12）

- P0-P3 + 缺口补齐 + UI 原型/Widget 库 + addon2 后端增强全部完成。
- 后端 addon2：A/B/C/D 四阶段全落地，68 用例 516 断言通过。
- 等：emscripten（编 wasm 验证）、WhaleUI 纯 C API（接 WhaleDom）。

<18>(SOLVED): UI 跑通（web 原型 + C++ Widget + wasm 内嵌 + 本地 HTTP 服务）。
  - 教训：上一轮把标签页模式与侧边栏模式杂糅、样式做成 Material 风（药丸/大圆角），
    用户明确讨厌 Material Design。重做：顶部标签栏单一导航模式；桌面原生风格
    （直角、1px 边框、标准控件）；prefers-color-scheme 深浅色。
  - web/：index.html（wasm 启动壳：#fs-root + 加载 fs_ui.js）+ style.css（桌面风）+ glue.js
    （DOM 适配层，经 --post-js 打进 fs_ui.js）。
  - src/ui：IDom 兼容层（NullDom/BrowserDom）、Signal/Effect 响应式、21 控件 Widget 库、
    IUiBackend 抽象（桌面 NativeBackend 进程内直调预留）、UiApp（顶部标签页模式）、
    ui_server（httplib 从内存提供内嵌资产）、ui_main_wasm（wasm 入口）。
  - 构建流程（scripts/build_ui.bat，无硬编码路径，只依赖 PATH 里的 emcc/xmake/python）：
    xmake -p wasm 编 fs_ui_wasm -> web/fs_ui.{js,wasm} -> scripts/gen_assets.py 打包成
    src/ui/ui_assets_gen.hpp（C 字节数组）-> xmake -p windows 编 flashsight.exe（内嵌资产，
    运行时起本地 HTTP 服务并开浏览器）。WhaleUI 就位后换 UI 后端即可，逻辑不变。
  - 关键 bug：BrowserDom 的 next_id_ 从 1 起，与 glue.js 保留的 root id=1 冲突，
    首个 create 覆盖 root 映射导致 appendChild 自己 append 自己（HierarchyRequestError，
    页面全空）。改从 2 起 + glue.js 加自环/祖先环防护。
  - emcc 6.0.12 在 C:\Program Files\emscripten（xmake 的 emcc 工具链可用；
    默认 -fvisibility=hidden 会隐藏 extern "C" 入口，需 -fvisibility=default）。
  - 已知：响应式框架较粗糙，后续大概率大规模重构（用户明确）。

<19>(SOLVED): 按 rules/addon3.md 重做 UI 框架 + 构建流程（用户："你做的太烂了"）。
  分层重构为四层（addon3 架构）：
  - DOM Adapter：ui_dom.hpp 重写为完整 IDom（create_element/create_text/append/
    insert_before/remove/clear/set_text/set_attr/remove_attr/set_style/set_visible/
    get_value/on/off/root）。NullDom（带树模型 + dump()，测试断言）供 native/测试；
    WasmDom（ui_dom_wasm.cpp，emscripten/val.h + EM_JS 事件绑定）供 wasm。
    WhaleUI 就绪后加 WhaleDom 即可，上层不动。
  - Framework：ui_vdom.hpp/cpp —— VNode 树 + H 流式构建器 + EventBus（整型 handler id）
    + keyed diff/patch（后向遍历 insertBefore 重排；事件 handler 变化时先 off 再 on，
    否则每次渲染都叠加旧监听器导致一次点击触发多个 handler）。
  - Composers：ui_components.hpp/cpp —— button/title_button（探索/项目/用量：仅颜色+
    下划线，无背景）/tab_strip（可关闭+加号）/panel_tabs（自由侧栏标签）/
    chat_message（user=气泡、assistant=文章无气泡、system=居中浅字）/
    tool_block（折叠流程块）/turn_summary（不折叠无边框）/markdown_blocks（标题/列表/
    围栏代码/行内 code·bold·italic）/modal/menu 等。
  - user：ui_app.cpp 重写 —— 顶部可增删标签条（主页/会话N/设置）；主页居中标题按钮 +
    项目卡片（左信息右可展开会话列表）+ 用量图表/表格；会话页文章式对话 + 自由侧栏
    标签 + 终端选择器抽屉；设置页两栏。整个 AppState 一次性重渲染 + patch。
  - 关键 bug：① xmake 的 io.readfile 是文本模式，会把二进制 wasm 的 UTF-8 字节替换坏
    （header 长度 400466 vs 真实 314151，wasm 魔数被毁）→ 必须 io.open(...,"rb") 读、
    io.open(...,"wb") 写；② val.h 需要 C++17（wasm target 用 c++17，native 仍 c++14）；
    ③ xmake 的 import()/io 只在 target scope 可见，文件顶层用 os.getenv/os.isfile。

<20>(SOLVED): 构建流程改为纯 xmake（删掉 scripts/build_ui.bat 与 gen_assets.py）。
  - fs_ui_wasm 用 set_plat("wasm")+set_toolchains("emcc") 作为 fs_ui 的 dep
    （add_deps("fs_ui_wasm", {link=false})），一条 `xmake` 自动先编 wasm 再编 native。
  - 资源打包在 fs_ui_wasm 的 after_build 里用 Lua 直接生成 src/ui/ui_assets_gen.hpp
    （web/index.html+style.css+fs_ui.js+fs_ui.wasm；glue.js 已由 --post-js 内联，
    不再单独存在）。无硬编码路径、无外部脚本、跨平台。
  - 事件端到端已验证（浏览器）：tab 切换、项目展开、开会话、侧栏切子代理/终端、
    输入+发送、规划面板、用量图表、模态全部有响应；86 用例 578 断言通过。
  - 新增 test_ui.cpp（DOM/diff/组件）+ test_ui_app.cpp（App 集成）替换旧 UI 测试。

<21>(SOLVED): 终端接线（reproc，rules/user.md）。
  - 引入 3rdparty/reproc v14.2.5（MIT，只用 C 源+头；平台文件由 xmake 按 OS 选）。
  - src/core/terminal_manager.{hpp,cpp}：reproc 封装 spawn/poll/kill；前台 exec 超时
    自动转后台；输出累积（上限 1MB）；shell 经 ShellDetector 解析（cmd/pwsh/bash 参数不同）。
  - FsApi 构造 TerminalManager 并注入 AgentHostDeps 的 terminal_spawn/poll/kill；
    make_ctx 补齐 sid（注册表按会话归属终端）+ terminal/net 回调。
  - network.fetch 回调也接上（NetClient，method/headers/body/timeoutMs/sizeCap/verifyCert）。
  - vision.screenshot/readImage 改为明确的 not-supported（之前经 net_fetch 转发是错的：
    net 接线后它会真发网络请求）；需要平台截图/图像解码后端，暂缓。
  - 新增 tests/test_terminal.cpp（真实起 shell 验证输出/kill）；86 用例 578 断言通过。

<22>(SOLVED): 纯 xmake 构建的依赖顺序坑（addon3 §构建的落地修正）。
  - 症状：`xmake` 一条命令时，fs_ui 的编译在 fs_ui_wasm 产出 web/fs_ui.js 之前就开始，
    生成的 ui_assets_gen.hpp 还没写出来 -> `ui_server.cpp:8 fatal error: ui_assets_gen.hpp`。
    原因：xmake 默认跨 target 并行编译（build.across_targets_in_parallel=true），
    add_deps 只保证链接顺序，不保证"依赖目标的产物先于本目标编译"。
  - 解决：`set_policy("build.across_targets_in_parallel", false)` 串行化 target，
    使 add_deps 成为真正的编译顺序边；资源打包仍放在 fs_ui_wasm 的 after_build 里。
  - 另：wasm target 必须 `set_extension(".js")`，否则 emcc 默认产出单个自包含 .html，
    没有独立的 js/wasm 供打包（原来靠手工删掉 .html 才对，现在直接产 .js+.wasm）。
  - 验证：删掉 build/.xmake/web 产物 + ui_assets_gen.hpp 后，单条 `xmake` 从零构建成功，
    86 用例 578 断言通过，浏览器渲染正常。




<23>(SOLVED): 用户反馈第一批（可用性）修复 + 两个底层真 bug。
  反馈项：① 设置项与左侧导航不对应且不全；② 项目卡片给会话的位置太小、两个按钮语义混乱；
  ③ 打开文件夹不该手输路径（要系统选择框/拖拽）；④ 会话 Enter 发送后输入框不清空；
  ⑤ 挂起改普通按钮且挨着发送；⑥ 去掉 switch 工具栏，功能移到输入框下；
  ⑦ 侧栏不能拖拽调宽；⑧ 侧栏“+”不该默认文件页。
  - 设置：导航改为字符串键（kSettingsNav 12 项，nav 与 body 用同一 key，不可能错位）；
    settings.dump 一次拉全量（带来源），sv_bool/sv_int/sv_str 读缓存；子代理/MCP 与 Skill/
    长期记忆/网络/终端/文件/环境/快捷键/关于全部有实体内容。
  - 项目卡片：改为“上信息行 + 下宽会话区”，会话列表占卡片主体；按钮改为
    「在资源管理器中打开」（project.reveal）与「移出列表」；删掉手输路径行。
  - 打开文件夹：新增 core/folder_picker（Windows IFileDialog FOS_PICKFOLDERS；POSIX stub），
    FsApi project.pickFolder/project.resolveDrop/project.reveal；浏览器拖入文件夹只能拿到
    名字（webkitGetAsEntry().name），后端在 USERPROFILE+固定盘 BFS 查找（跳噪音目录，深度6）。
  - 输入框：textarea 的 value 必须用 attr("value")（活属性），发送后对 DOM 节点直接写
    value=""（打字期间不重渲染，状态清空不会反映到真实字段）。
  - 侧栏：新增 .side-splitter 拖拽条（EM_JS mousedown/move/up，220–720px，内联宽度，
    patch 不碰 inline 尺寸所以重渲染不丢）；“+”改为弹出种类菜单（open_or_focus_side_tab
    同类复用，不再默认 files）。
  - 真 bug 1（EventBus::fire 自毁）：handler 里 update()->render()->retain() 会 erase 正在
    执行的 map 条目（闭包捕获随之下析构）→ 后续语句读到悬空捕获，表现为“点了没反应/半执行”。
    修复：fire() 先拷贝 EventFn 再调用。
  - 真 bug 2（session 解析依赖内存映射）：SessionManager::project_store_of 只查 sid_pid_
    内存表，服务重启后（或本进程没开过的会话）一律解析失败 → session.list 静默丢会话、
    session.get 报 not-found。修复：未命中时遍历项目注册表找含该 sid 的项目并回填缓存；
    list_sessions 直接走项目 store，不再 make_store(sid)。
  - 构建：folder_picker.cpp 的注释行尾是反斜杠（`// drive roots C:\..Z:\`）→ 续行把下一行
    的 DWORD drives 声明吞进注释；ole32/uuid 需加入 add_syslinks。
  - 验证：99 用例 642 断言通过；浏览器回归：项目卡片会话区、真实键盘 Enter 发送+清空、
    侧栏拖拽 320→220px、侧栏“+”菜单选择与复用、拖放文件夹 resolveDrop 命中、
    设置 12 节全部渲染、旧会话打开（session.get 修复后）。


<24>(SOLVED): 用户反馈第二批（布局/方向/设置做实）。
  - 项目卡片改双布局：宽屏 grid 左 1/3 信息 + 右 2/3 会话（默认），
    窄屏（@media max-width:900px）自动堆叠为上行信息 + 下全会话区。
  - 侧栏拖拽方向修正：分隔条在侧栏左缘，往左应加宽 -> 位移取负号（原来写反）。
  - 提示（toast）自动消失：AppState 加 toast_deadline_ms，pump()（250ms 定时器）
    过期清除；wasm 下 now_ms 用 emscripten_get_now()（std::chrono::steady_clock
    在 emscripten 上取值不可靠，导致 deadline 永不到期）。
  - 供应商管理做实：FsApi provider.templates/getProvider；provider_add_provider
    编辑时若 apiKey 为空或含 **** 则保留原密钥（否则掩码会覆盖真密钥）；
    UI 左列表 + 右详情，新建先选模板再填表；自定义 header 可增删；格式可选
    （openai-chat/openai-response/anthropic/plan/custom）；测试连接/获取模型列表/删除。
  - x-opencode-session 透传：ModelCallRequest.session_id -> ChatRequest.session_id
    （agent_loop 用 opts_.sid 填充），plan 形态自动作为亲和头发送。
  - MCP/Skill 做实：McpRegistry upsert/set_enabled/remove/dump/load；FsApi mcp.list/
    import/add/setEnabled/remove/detect（探测 ~/.claude/skills、~/.config/opencode/skills 等）；
    注册表持久化到设置 mcp.entries（重启恢复）；UI 列出、启停、新增（含目录浏览）、移除。
  - 设置补齐：新增「会话默认」「浏览器控制」「电脑控制」分区；外观加字体族/字号/等宽字体；
    网络加代理主机/端口（HttplibNetClient::set_default_proxy，设置变更立即生效）；
    文件加黑/白名单多行编辑（DenyList 装载 + 工具层 deny_check 拦截）；
    快捷键改为可编辑输入框。
  - 项目列表带 sessionCount（ProjectManager::list_projects 统计会话目录数）。
  - 验证：102 用例 658 断言通过；浏览器回归：卡片双布局切换、侧栏左拖加宽、
    提示 3.2s 消失、供应商模板向导/编辑/密钥保留、MCP 探测 72 个工具包并持久化启停、
    新设置分区逐项渲染。


<25>(SOLVED): 反馈第二批续 —— 侧栏做实（文件树/操作/调用）、key-value 表可编辑、/ 命令。
  - 长期记忆/环境变量/CLI 工具三张表加「添加/删除」行；sv_obj 读取 {value,source}
    包装形态（此前设置项读不到值、写入后不刷新）；set_setting 后重派生缓存并重渲染。
  - 侧栏文件树接线：FsApi project_files/project_read_file（safe_project_path 拒绝越界，
    >512KB 拒绝预览）；UI 面包屑/上级/刷新、目录进入、文件预览；会话打开记录 active_pid。
  - 操作记录面板读 session.get 的 systemOps；调用记录面板读各 turn 的 toolCalls。
  - 输入框「+ 文件」打开文件树；「/ 命令」弹 skill 列表，选择写入 /<name>。
  - git 面板给出明确未接线说明（git 工具尚未实现）。
  - 注：model_table 的预置表里 Anthropic 三条模型 id 为空（历史遗留），本轮未动。
  - 验证：102 用例 658 断言通过；浏览器实测文件树进出、/ 命令选择、两个新面板。


<26>(SOLVED): 框架层修复（用户反馈第三批）。
  - 会话状态按标签隔离：新增 SessionView + save/load_session_view + switch_tab。
    此前所有会话共用一组扁平字段（messages/side_tabs/文件树/侧栏宽度…），
    开新会话会清空旧会话、切回变空。现在每个会话标签有自己的完整视图，
    切换时把当前视图存回 views_[sid]、恢复目标视图；首次进入才拉历史。
    show()/open_session() 统一走 switch_tab；load_session 回包只写回它自己的
    视图（避免慢响应覆盖别的标签）。
  - 后台会话事件隔离：payload.sid 非当前标签时写进该会话的视图（子代理/终端/
    流式都按 sid 归属），不再污染当前页。
  - 侧栏宽度持久化：bind_splitter 加 on_resize 回调（EM_JS mouseup ->
    fs_ui_split -> C++），拖动结束把宽度写进 state 与当前视图；侧栏宽度由
    状态驱动（inline style），节点重建/切标签都不丢。
  - 动画：H::anim(cls)/add_class + CSS 关键帧。页面切换 anim-page（0.16s
    淡入+微上移），文字/行出现 anim-fade（0.05s 淡入），弹层 anim-pop
    （0.12s）。只挂载时触发（diff 复用节点不会重播），并尊重
    prefers-reduced-motion。
  - skill 选择器：从"输入框下方撑开"改为绝对定位浮层，弹在输入框上方，
    不挤压输入框；新增透明遮罩 + Esc（document 级 keydown，经 fs_ui_key 导出）
    可收回；pump 定时器不再依赖。
  - 框架通用性：IDom 新增 bind_splitter(on_resize) / bind_global_key；
    NullDom 记录并支持 fire_split_for_test/fire_key_for_test，wasm 侧 EM_JS
    实现，两边接口一致（WhaleDom 接入时按同一签名实现即可）。
  - 验证：106 用例 674 断言通过；浏览器实测 A/B 两会话各自的消息与侧栏宽度
    互不干扰、切回 A 保留 history-marker 与 460px 侧栏、Escape/遮罩收回选择器、
    anim-page 生效。


<27>(SOLVED): 反馈第四批 —— 框架作用域通用化、会话页重排、供应商格式、终端后台推送。
  - 框架：新增 KeyedStore<T>（ui_vdom.hpp）——"作用域状态"通用原语：应用只保留
    一份 live 状态（AppState 的扁平字段），其余按 scope key 暂存在 KeyedStore。
    会话标签是第一个使用者（SessionView），后续任何可切换作用域（面板/工作区）
    复用同一机制，不再为每个场景写特判的 save/restore。
  - 会话页重排（按用户要求）：上栏 = [计划浮窗] + [中间历史+输入框] + [分隔条] +
    [右侧栏]，下栏 = 终端。历史区居中 50% 宽（.history-inner），输入框与之对齐；
    计划面板改为绝对定位浮窗贴左上，不再把对话往下推；侧栏可由分隔条上的把手
    收起/展开（side_collapsed 入状态，跨会话保留）。
  - 输入框：内嵌 textarea 去掉自身边框/聚焦环（此前与 composer 的 :focus-within
    双重激活，边框重复）；工具栏按钮 white-space:nowrap 防止文字竖排换行。
  - 供应商：格式改为固定下拉（OpenAI Chat / OpenAI Response / Anthropic / 套餐 /
    自定义）；模板只作 endpoint + 特殊 header 预设，选中后直接跳到表单；
    新增"高级"折叠区（自定义 header + 模型获取，user>api>preset>保守缺省）；
    新建/编辑共用同一表单构建。
  - 终端：后端非阻塞轮询（TerminalManager::tick 只发新增字节 + 退出标记），
    桌面端 FsApi 起后台线程，浏览器端 UI 定时器调 ui.terminal.tick；结果推成
    terminal.output / terminal.exit 事件，UI 不再有"轮询"按钮，输出自动追加。
    两端的差异只在"谁驱动 tick"，推送路径一致（同一 emit_event）。
  - 动画：分三档并只挂载时触发——view 切换 anim-view(.18s)、组件出现
    anim-enter(.14s，消息/卡片/行)、流式文本 anim-fade(.05s)；侧栏展开
    anim-slide-left。H::anim 并入 class 属性（此前会推重复 class 属性，
    浏览器只认第一个，导致元素真实样式被丢——计划浮窗曾因此错位）。
  - 验证：106 用例 679 断言通过；浏览器实测新布局、计划浮窗贴左上、侧栏拖拽
    不再误收起、供应商模板->表单+格式下拉+高级、终端输出自动推送（events 里
    可见 terminal.output/terminal.exit）。


<28>(SOLVED): 反馈第五批 —— 终端高度/交互式终端/拖拽识别/i18n 预留 + 两个真 bug。
  - 终端栏加高 + 垂直拖拽：默认 340px（原来输出区上限 160-220px 太矮），
    顶部新增 .td-resize 拖拽条（IDom::bind_vsplitter，EM_JS fs_dom_vsplitter，
    120px..85% 视口，向上拖变高）；高度入 state/SessionView（term_height），
    节点重建不丢。终端栏右侧状态文字删掉（用户反馈无意义），改成标题旁的小徽标
    （运行数/总数），无终端时不显示。
  - 新建终端 = “设置里的 shell”，不是手输命令：表单改为 shell 下拉
    （settings.detectShells 结果）+ 可选名称，terminal.spawn（interactive=true）
    起交互式会话（cmd /Q、pwsh -NoLogo -NoProfile、bash -i；stdin=pipe）；
    新增 terminal.write 把输入行写进 stdin，输出区下方有输入行
    （仅单终端视图且 running 时可用，Enter 发送、发完清空；聚合视图无单 stdin）。
    TerminalRegistry 加 label 字段（用户命名，to_json 透出，UI 标签显示 #tid 名称）。
  - 拖拽识别：bind_drop_zone 回调改为 (kind, value)。wasm 侧
    webkitGetAsEntry().isDirectory 分目录/文件；text/uri-list 判 URL；
    text/plain 兜底。dir -> 打开项目（resolveDrop）；file -> 输入框插 @名字；
    url/text -> 插入输入框（home/settings 页给出提示不静默）。
  - i18n 预留：fs_i18n 从空表改为可用实现 —— 源码中文即查表 key，
    tr() 中文原样返回、英文查 en 表（缺条目回退中文，不泄漏 key）；
    settings ui.language（auto/zh/en）经 SettingsStore::sync_language +
    UI set_setting/apply_settings 双通道即时生效；系统语言探测
    （Windows GetUserDefaultUILanguage / wasm navigator.language / POSIX LANG）。
    UI 已接入：导航/分组、外观语言项、终端栏/表单、toast、拖拽提示等。
  - 真 bug 3（终端输出非 UTF-8 让 /api/events 500）：cmd.exe 在中文 Windows 按
    OEM 代码页（GBK）输出，字节直进 JSON dump() 抛 type_error.316 →
    /api/events 返回 500，UI 事件流整个断掉。修复：新增 common/fs_encoding
    （decode_output：合法 UTF-8 原样；否则按 OEM/GBK 转；跨 read 的半个多字节
    序列留 pending 下轮补齐；sanitize_utf8 兜底替换符），drain 走它；
    terminal.write 反向 encode_input（中文命令按代码页转回去）；
    ui_server /api/events dump 加 try/catch 兜底（坏 payload 不再能卡死 UI）。
  - 真 bug 4（tick 双驱动数据竞争）：桌面端后台 poller 线程与浏览器 UI 定时器
    都调 terminal.tick → TerminalManager 无锁并发碰同一 reproc 句柄，出现
    重复 terminal.exit、终端假退出。修复：TerminalManager 全部入口
    （spawn/poll/kill/write/tick）加 mu_ 互斥。
  - 真 bug 5（悬空 pid）：项目注册表重建后老会话 meta.pid 悬空 →
    文件面板报 “path escapes project”。修复：session_get 自愈（meta.cwd 反查
    当前 pid 返回）；terminal_spawn 的默认 cwd 同样回退 meta.cwd。
  - 验证：112 用例 702 断言通过（新增交互式终端写读、GBK 解码/跨块、
    输入反向编码、拖拽分派、终端高度持久化、shell 选择表单、栏右侧文字删除）；
    浏览器实测：拖高 338→456px 持久、shell 下拉（自动探测/pwsh/powershell/cmd）
    起终端、输入“echo 交互输出测试”回车出结果、URL 拖入插链接 + toast、
    文件拖入插 @名字、■ 停止后输入禁用、语言切 English（导航/终端标题变英文）
    再切回中文、/api/events 中文输出正常（“版本/交互输出测试”）。


<29>(SOLVED): 反馈第六批 —— 真终端（VT 解析 + ConPTY）、动画框架化、卡片双布局、
  组件视觉重做 + 5 个真 bug。
  - 终端不再是文本流，是「屏幕」：新增 src/term/fs_vt（VT/ANSI 解析器 + 单元格
    网格）。C0/CSI/OSC/DEC 图形字符集/UTF-8（含 CJK 双宽）/备用屏幕/DECCKM/
    bracketed paste/DSR 回包（shell 问光标位置，不回会卡住）全部支持；连续同样式
    单元格合并成 run 再交给 UI，一屏通常几十个节点而不是 cols*rows 个。
    UI 按列定位（left: Nch）画格子 + 光标块，不再 pre 直接贴字节流（以前 
    会漏到界面上）。18 个单测覆盖解析语义（换行不回车、自动换行、滚动区、
    插删行列、SGR 256/真彩、备用屏幕、跨 read 拆包、宽字符、DEC 图形）。
  - 真终端（ConPTY）：新增 src/core/conpty（Windows 伪控制台，Win10 1809+）。
    管道给不了窗口大小、提示符和全屏程序，用户终端改走 ConPTY：子进程拿到真
    控制台，我们拿 VT 字节流 + 按键。可用时 terminal=conpty，否则退回管道
    （输出仍进同一个屏幕模型）。真 bug：**必须设 STARTF_USESTDHANDLES**——
    否则子进程继承父进程的控制台句柄，输出跑到我们自己的控制台，伪控制台只吐
    启动序列、shell 立刻退出。这个是逐项对比试出来的（见 temp/conpty_std.cpp）。
  - 按键是字节，不是文本：IDom 新增 bind_key_input(KeyPress)/focus/bind_cell_size；
    KeyPress 带修饰键。fs_vt::encode_key 负责终端语义（Ctrl+C=0x03、Shift+Tab=
    ESC[Z、方向键按 DECCKM 在 SS3/CSI 间切换、Alt=ESC 前缀）；encode_paste 按
    bracketed paste 包 ESC[200~/201~。终端的 <div> 自己吃键盘并阻止浏览器默认
    行为（否则 Ctrl+W 关标签页）。
    真 bug：**按键乱序** —— 写入走异步 HTTP，快速输入时后一个键会超车前一个
    （实测 "d" 跑到 Enter 后面）。改为客户端队列 + 单飞行（term_key_queue_/
    term_key_inflight_），上一个写完才发下一个。
    真 bug：**光标不对齐** —— 光标是行的兄弟节点、用 top: Nem 定位，而行高是
    1.28em、em 指的是字号、面板还有 padding，必然偏。改为放进它所在的行里。
  - 终端实时刷新：屏幕在后端（解析器在那儿），pump() 每拍重取可见终端的网格
    （带单飞行保护）；不再只靠 terminal.output 事件（漏一个事件面板就停更，
    这正是"只有点进去才刷新"的原因）。运行中的终端才轮询，退出的自动停。
  - 新建终端一键直创：不再有表单（shell 取设置里的默认 shell，名字在标签上改）。
    标签上加 ✎ 重命名；**失焦即提交**（新增 kBlur 事件），输入框不会一直吃键。
  - 终端「所有终端」：不再是拼接的裸文本（以前会显示 ），改成每个终端一张
    卡片（当前屏幕文本 + 状态），**点击即跳到该终端**（之前只能看不能跳）。
  - 终端收起不再撑高度：真 bug —— patch 只设新样式、从不清除被删掉的 inline
    style，收起的抽屉一直保留 open 时写的 height。修复：patch_attrs 对被删除的
    style 显式置空（框架级，任何组件都受益）。
  - 动画框架化：新增 anim 注册表（ui_vdom）——动画按「意图」声明
    （view/enter/list/pop/fade/slide-left/slide-right/drawer），框架映射到 class，
    未注册的意图不生效（拼错不会产生死 class，测试可查）。补齐 CSS：新增
    anim-list（列表逐行出现）与 anim-drawer（抽屉展开），并把列表/行类组件
    （td-all-item/ts-row 等）纳入统一定义，不再靠"谁记得加 class"。
  - 项目卡片双布局（按卡片自身宽度，容器查询）：宽屏**直接显示会话列表**并
    可滚动（max-height 300px，没有展开/收起），卡片不再被 18 个会话撑大、
    左侧也不再空；窄屏才堆叠 + 展开收起。列表改为始终构建（布局交给 CSS），
    项目列表回来后为每张卡片预取会话。修了一个 CSS 优先级坑：基础折叠规则
    比 min-width 容器查询更"晚"生效，导致宽屏也折叠——改为把折叠放进
    max-width 容器查询里。
  - 组件视觉重做（从组件开始，按用户要求）：调色板/圆角/阴影/字体变量重做
    （更中性的灰阶 + 单一强调色 + 6px 圆角 + 真实阴影 + Inter/Cascadia Mono）；
    按钮改扁平填充 + 发丝边框 + 焦点环（去掉渐变和斜面，hover 只变表面不变
    字重，避免光标下跳动）；输入框/下拉加 hover 态与自绘 chevron；开关从胶囊
    改成方形轨道（呼应"不要胶囊"）；顶部标签从文件夹页签改成圆角 chip；
    section-title/field/badge 对齐到新尺度。
  - 验证：141 用例 843 断言通过（新增 VT 18 例、动画注册表 3 例、样式清除 1 例、
    终端网格/按键/尺寸/重命名 4 例）。浏览器实测：终端输入 abc 200ms 内自动
    出现（不点任何地方）、光标与行对齐（cursorInRow=true 且 top 相同）、收起
    340px→29px 且 inline height 清空、所有终端卡片可点击跳转、重命名失焦即
    提交、卡片宽屏 339px 可滚动列表无 toggle / 窄屏 665px 折叠且有展开按钮。
