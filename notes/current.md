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


<30>(SOLVED): 反馈第七批 —— 终端跟手、文件上传、终端概览/关闭、侧栏加号页面、
  动画按页面声明。
  - 终端跟手：轮询拆成两条定时器——事件泵仍 250ms，终端网格另起一条 50ms 的
    （tick_terminal）。之前跟事件泵共用 250ms，长按一个键就是"一批批跳出来"。
    桌面端后台 poller 也从 400ms 提到 60ms。加"内容没变就不重渲染"的判断
    （grid 逐字节比较），空转终端不产生渲染。实测连敲 8 个字符逐个出现
    （60ms 采样：a → ab → abc …），顺序正确（abcdefgh 完整）。
  - 「文件」按钮改成上传：新增 IDom::bind_file_picker（点按钮开系统文件选择器，
    JS 侧 multipart POST /api/upload，C++ 只收结果）；服务端新增 /api/upload
    + FsApi::project_save_upload，文件落到 <project>/.flashsight/uploads/
    （放项目里是为了让模型的普通文件工具能按路径读到；同名加 -1/-2 后缀不覆盖，
    文件名只取基名并清洗 Windows 保留字符，防目录穿越）。上传完把相对路径以
    @path 插进输入框并提示。之前这个按钮只是打开侧栏文件树，语义完全不对。
  - 终端概览（所有终端）：每张卡片只显示**最近 5 行**（term_grid_tail），
    卡片小、列表可滚动；概览用 round-robin 每拍刷一个终端（term_grid_rr_），
    终端再多请求速率也有上限。实测两个终端各显示自己的 tail。
  - 终端标签可关闭：新增 × 「关闭标签（终端继续运行）」——只从标签条移除、
    进程照跑（模型可能还在用），并记进 term_hidden 防止刷新又冒出来；
    只有 ■ 停止才真的结束进程。实测关闭后 terminal.list 里进程仍是 running。
  - 侧栏「+」从右侧弹出菜单改成**临时页面**（.side-add）：列出全部面板种类，
    每个带一句说明（"项目文件树与预览"/"模型发起的工具调用"…），已打开的打
    「已打开」标记并点击直接跳过去。菜单塞不下一句话说明，页面可以。
  - 动画按页面声明：新增页面动画表 kPageAnims（page -> 意图），build_shell 按
    page_id 查表应用，不再由某个 builder "记得"加 class；终端抽屉的展开动画
    也改为在构建处声明（anim::kDrawer）。新增测试断言 home/settings/session
    三个页面都带 anim-view（漏声明就红）。
  - 验证：146 用例 1519 断言通过（新增 5 例：页面动画覆盖、关闭标签保留进程、
    文件上传接线、加号页面、概览 tail）。浏览器实测：连敲 8 字符逐个出现且顺序
    正确、× 关闭后进程仍 running、上传落盘（重名得 -1）、概览两卡各显示 tail、
    加号页面 7 张卡 + 3 个「已打开」标记。


<31>(SOLVED): 反馈第八批 —— 终端可滚动（scrollback）+ 切换终端动画补齐。
  - 终端 scrollback：Screen 新增历史缓冲（每行一个 Cell 向量，上限 2000 行），
    滚出顶部的行进历史；只有「滚动区从第 0 行开始」才记历史（DECSTBM 从第 2 行
    开始是程序在窗口里画东西，不算历史），备用屏幕不记（全屏程序自己的画布）。
    total_lines()/line_cell() 把历史 + 可见屏幕拼成一个可寻址缓冲，UI 直接当
    滚动容器用。grid 接口加 from/scrollback/totalLines（可只取一段历史，
    上限 1200 行，避免 50ms 轮询时每次传 2000 行）。UI：.term-screen 改
    overflow:auto，行放进 .ts-lines（keyed by tid）。
    之前终端根本不能滚（overflow:hidden），看不到刚过去的输出。
  - 滚动跟随：新增 IDom::bind_scroll_pin/scroll_to_bottom —— 视图在底部时新输出
    自动跟随，用户往上翻看历史时绝不被拽回底部（实测翻到顶后继续输出 scrollTop
    仍为 0；在底部时跟随成功）。
  - 切换终端动画补齐（用户指出的"切第二个有动画、第一个没有"）：根因是行节点
    被 diff 复用 —— 切换后若新终端行数不更少，节点全部匹配上、没有任何节点被
    创建，挂载动画自然不触发（这是"挂载时才播"的正确行为，但切换场景需要显式
    声明）。修法：给行容器一个按 tid 的 key（term-lines:<tid>），切终端即换子树
    从而重建、动画必然触发。实测 #1/#2 切换后都是 ts-lines anim-enter。
  - 验证：152 用例 1543 断言通过（新增 4 例：scrollback 保留/上限与滚动区、
    reset 清历史、备用屏幕无历史、UI 历史渲染+光标落在合并缓冲的正确行、
    滚动 pin 状态）。浏览器实测：40 行输出后 scrollHeight 690 > clientHeight 307
    可滚动、翻顶后输出不被拽回、底部时自动跟随、切换两个终端都带 anim-enter。


<32>(SOLVED): 反馈第九批 —— 终端列表 keyed（修关闭串扰）、终端搜索、不加终端。
  - 真 bug（关闭标签会"顺手新建一个"）：终端标签条是**无 key 列表**，diff 按位置
    匹配，所以删掉一个标签后它的 DOM 节点被下一个条目复用；而那次点击仍在冒泡，
    于是落在了复用后那个元素（新增）的处理器上 —— 关掉最后一个终端就立刻新建了
    一个。修复：标签条所有条目都加 key（term-tab:all / term-tab:<tid> /
    term-tab:add），删一个不会让别的条目被复用。新增测试断言这些 key 存在
    （key 是框架级身份、不进 DOM，只能从 VNode 树断言，为此加了 tree_for_test()）。
    实测连续关闭 11 个终端，最后剩「所有终端 + 新增」，没有自动新建。
  - 终端搜索：终端数 >= 10 时在「所有终端」页出现筛选框（少的时候不显示，避免
    噪音）。输入实时筛选（每次按键都 update 重渲染）。三种匹配：
      * 名称（始终开）：标签名 / shell / #id
      * 输入匹配（可切）：该终端**敲过的命令**（新增 term_input_log 记录按键，
        退格会删、回车换行）
      * 全文匹配（可切）：整屏内容（含输出）
    带正则元字符的模式按正则（忽略大小写）；正在敲一半的正则（如 "foo["）
    编译失败时把元字符去掉再模糊匹配 —— 半截模式把列表清空是最烦的行为；
    其余按模糊子序列（"bld" 能匹配 "build-log"）。右侧两个模式开关因栏窄只显示
    图标，说明放 title（hover 提示）。无匹配时给明确空状态而不是白板。
  - 验证：155 用例 1549 断言通过（新增 3 例：筛选三模式与阈值、实时筛选、
    标签 key）。浏览器实测：11 个终端时搜索框出现（占位符 + 两个图标开关 title
    正确）、逐字符输入 b/bl/bld/build 卡片数 3→2→2→1 且焦点不丢、清空恢复 11、
    连关 11 个不自动新建。


<33>(SOLVED): 反馈第十批 —— 标签条不再被筛选影响、新建终端一定可见、关闭有回路、
  输入框焦点保护。
  - 真 bug（新建终端没有标签页）：筛选被套用到了**标签条**上，于是新建的终端
    名字不匹配当前筛选时，它的标签就不显示（用户完全测不下去）。修复：标签条
    永远列出全部终端，筛选只作用于「所有终端」页的卡片（那才是搜索的地方）；
    另外新建终端成功后清掉筛选 —— 你刚要求建一个终端，就必须能看见它。
  - 关闭终端不再是无回路操作：关闭只是隐藏（进程还在跑），所以标签条底部新增
    「已关闭 (n)」入口，点一下把关闭的标签全部恢复（term_hidden 清空 + 重新拉
    列表）。之前关掉就再也找不回来。
  - 真 bug（输入框因列表重建失去焦点）：composer 故意不在每次按键时重渲染
    （否则会跟光标打架），所以状态里的值在打字期间是旧的；后台重渲染（终端
    50ms tick）会把这个旧值写回 DOM，光标随之重置。修复在框架层：新增
    IDom::is_focused(node)，patch_attrs 对**获得焦点**的控件的 value 跳过写入
    —— 聚焦控件的值属于用户。这样任何后台重渲染都不会打断打字。
    实测：终端持续刷屏时 composer 与筛选框连续打字，节点身份不变、焦点不丢、
    文本不被打回。
  - 验证：157 用例 1572 断言通过（新增：标签条不受筛选影响、聚焦输入框保留用户
    文本、关闭标签可恢复）。浏览器实测：筛选状态下点新增 → 标签出现且筛选被清、
    标签条列出全部 12 个终端、关闭 3 个后剩 all+新增 且「已关闭 (3)」可恢复、
    输出期间输入焦点稳定。


<34>(SOLVED): 反馈第十一批 —— 关闭的终端并入概览、侧栏去掉终端面板、面板可多开、
  侧栏视觉重做。
  - 关闭的终端不再单独做「已关闭」入口：终端本来就一直在列表里（关闭只移除标签，
    进程照跑），所以「所有终端」页里它照样是一张卡片，只是状态显示「标签已关闭」，
    点它就把标签恢复回来。之前那个独立入口是多余的。
  - 侧栏不再有「终端」面板：终端已经在底部抽屉里（且列出全部终端），侧栏再来一份
    是更差的重复。默认侧栏只留「用量」「子代理」，加号页面也不再列终端。
  - 面板可多开：新增 SideKind 表，带 multi 标记——文件/Git 可多开（两个文件面板
    就是两个目录），用量/子代理是单例（点已打开的直接聚焦，不重复）。为此把文件
    树状态从 AppState 扁平字段挪进 SideTab（path/entries/preview 各面板一份），
    load_files/preview_file 都带 panel id；实测两个文件面板各自进不同目录互不影响
    （一个在 /，另一个在 /.flashsight）。
  - 侧栏视觉重做：面板标签从「文件夹页签 + 底部强调线」改成与顶栏一致的圆角 chip
    （hover 变底色、active 用强调色底）；文件行/操作行/预览头/子代理卡片统一到
    新调色板（圆角、阴影、panel-2 底色），操作/调用行从裸分隔线改成卡片式。
  - 验证：157 用例 1540 断言通过（更新：侧栏标签切换改用 ops 面板、加号页面
    多开/单例两种语义、关闭标签可从概览恢复）。浏览器实测：默认侧栏只有用量/
    子代理、加号页面 6 类且文件/Git 标注可多开、两个文件面板目录各自独立。


<35>(SOLVED): 反馈第十二批 —— 设置页动画、外观做实、通用页补齐、清理解释性文字、
  修侧栏收起后拖拽。
  - 真 bug（侧栏收起后拖拽）：分隔条的 mousedown 监听是"装一次"的，节点跨 patch
    存活，所以收起后旧监听还在，拖动仍会上报宽度——展开时那个宽度就成了面板宽度
    （看起来像"坏了"）。修复：JS 侧每次绑定都更新 bar.__fsSplitCb/__fsSplitSide，
    未绑定时直接 return；收起时不再绑定（并显式解绑），光标也不再是 col-resize。
  - 真 bug（composer 抽搐）：按钮在图标/文字之间切换时高度会变，越过阈值就重排。
    修复：工具条按钮固定行高（28px）+ 容器查询阈值从 700 提到 940（真正的"文字排
    得下"宽度），两个模式不再重叠。发送/挂起改为「有宽度时显示文字、窄时显示图标」
    （text-wide：图标在宽模式隐藏），不再是无条件图标+文字。
  - 动画被加载吞掉：新增 VNode::anim_key（重放令牌）+ IDom::play_animation。
    挂载动画只在节点被创建时播，keyed 节点跨 patch 存活就再也不会播——所以慢加载
    时动画在空页面上放完了，内容到了反而没动画。现在部件在内容就绪时改令牌即可
    重放：会话页在 history 到达后重放，设置页每个分区切换都重放（body 按 section
    做 anim_key）。动画注册表仍是"按意图声明"，未注册的意图不生效。
  - 设置页动画：settings-body 带 anim::kEnter + anim_key("sec:<分区>")，切换分区
    有进场动画（之前设置页整体没有动画）。
  - 外观做实（原来只有三个下拉/输入框）：调色板与深浅拆成两个维度——ui.theme 选
    配色（默认蓝/石板灰/森林绿/紫罗兰/琥珀橙），ui.mode 选浅色/深色/跟随系统，
    所以每套配色自动有浅深两面。CSS 改为变量驱动：data-mode 决定灰阶，palette-*
    覆盖色相，data-mode 由前端解析 auto 后写入（只剩"从未告知"时才用媒体查询）。
    字体/等宽字体/字号也真的生效（--font-user/--mono-user/--font-size-user），
    字号改成 ± 步进（10–24px）。旧值 ui.theme=light/dark 仍兼容（当作模式）。
    真 bug：调色板 class 用 className 字符串拼装，每次调用都追加，累积成一串垃圾
    class（"t palette-fore t palette-forest …"）导致切换失效；改用 classList
    增删（幂等，不会重复）。
  - 通用页补齐（按 rules/goal.md「设置」节）：新增 界面显示（标签位置 顶部/左侧、
    会话名提示开关）、会话历史（保留轮数 / token 预算改 ± 步进，即时保存，去掉
    "保存"按钮）、用量统计（保留天数 ± 步进）。删掉一堆解释性文字（设置键名、
    上游工具路径、"尚未接线"之类）——那些属于 notes，不属于界面；只留真正有信息
    量的提示（优先级、"只影响新会话"、"留空 = 直连"）。关于页也精简为三项。
  - 验证：160 用例 1567 断言通过（新增：外观写入 DOM、分区切换动画重放、加载后
    重放动画）。浏览器实测：调色板逐个切换 accent 跟着变且 class 不再累积、
    字号步进写入 --font-size-user、外观页四个分区（主题/语言/字体/代码）、
    设置分区切换带 anim-enter。


<36>(SOLVED): 反馈第十三批 —— 设置面补齐（模型偏好/浏览器/电脑控制归位）、
  工具条不再换行、资源打包改为构建前（CSS 改动终于生效）。
  - 真 bug（CSS 改了不生效）：web/style.css 由 fs_ui_wasm 的 after_build 打进
    ui_assets_gen.hpp，而 xmake 是在"目标开始构建前"判定源文件新旧的——fs_ui
    检查 ui_assets_gen.hpp 时它刚被写完但已被判为"不新"，于是 CSS 改动永远到不了
    exe（表现为"我改了 CSS 但没用"）。修复：把打包挪到 fs_ui 的 before_build
    （在 fs_ui 自己的新旧判定之前写头文件），并把 web/style.css 等声明为该目标的
    header 输入以获得依赖边。注意 xmake 的 hook 是序列化的，文件级函数在 hook
    里不可见（"attempt to call a nil value"），所以打包代码直接内联在 hook 里。
    现在实测：改 CSS → xmake → 打包内容与 exe 都更新。
  - 工具条不再换行：flex-wrap 从 wrap 改 nowrap。两个档位（文字/仅图标）都按
    实际需要尺寸设计，wrap 只会带来意外换行——尤其是 .grow 这个 flex-grow 占位
    元素，行被判满时它会被挤到第二行，在工具条下方留一道空缝。发送/挂起改成
    text-wide（宽时只显示文字、窄时只显示图标），高度恒为 28px，越过阈值不再
    重排（这就是"抽搐"的来源）。实测 1000–2200px 全档位都是单行 28px。
  - 设置面归位（按 rules/goal.md「设置」节）：浏览器控制/电脑控制从「权限」挪到
    「通用」（文档里它们属于通用）；新增「模型偏好」分区（子代理/摘要模型的默认
    模型与思考档，provider.md §5）；子代理的模型下拉改为从已配置供应商的模型列表
    取（之前硬编码 gpt-4o-mini/claude-sonnet）；并行上限/嵌套深度改 ± 步进；
    关于页精简为版本/构建/界面三项。删掉更多内部解释性文字。
  - 验证：160 用例 1568 断言通过。浏览器实测：1000/1200/1400/1600/2000px 五档
    工具条均为单行 28px 且仅图标档图标齐全；收起侧栏后 composer 1092px 进入文字档
    （7 个标签显示、发送/挂起只显示文字）；设置 16 个分区逐一渲染正常。


<37>(SOLVED): 仓库卫生 —— build_check/ 与 build_whale/ 误入 git。
  - 上一次提交（反馈第十三批）把两个临时构建目录一起提交了：那是用
    `xmake f -o build_check` / `-o build_whale` 做交叉验证时留下的，共 ~480 个
    文件（构建缓存 + 依赖文件）。它们只是构建产物，已从索引移除并在 .gitignore
    里加了 build_*/ 规则（build/ 单独保留，因为它是最常用的那个）。
  - 教训：`git add -A` 在有多个 build 目录时会把它们全带上；临时构建目录一律
    用 build_ 前缀，规则才能覆盖。

<38>(SOLVED): WhaleUI 桌面 UI 接入 —— h/hpp 取回、WhaleDom + 原生入口、开关式编译。
  - 头文件：`3rdparty/whaleui/include/{whaleui.h,whaleui.hpp}`（WhaleUI include/ 副本；
    3rdparty 不入 git）。
  - 新增 `src/ui/ui_dom_whale.cpp`（WhaleDom：用 `whaleui_dom_*` 实现 IDom）与
    `src/ui/ui_main_whale.cpp`（原生入口：建 App/Window、注入 web/style.css +
    图标子集、挂 UiApp、事件循环）。`src/main.cpp` 按 `FS_UI_WHALEUI` 二选一。
  - 开关：`xmake f --whaleui=y [--whaleui_libdir=...] [--whaleui_pkgdir=...]`；
    默认 **关闭**（`ui_dom_whale.cpp`/`ui_main_whale.cpp` 整体在 `#ifdef` 内，
    `ui_server.cpp` 只在默认配置编译）。打开时跳过 fs_ui_wasm、链接
    libwhaleui-full + SDL3/SDL3_ttf 预编译包 + xrepo 的 lexbor/utf8proc/SDL3_image
    （从 xmake 包缓存直接找 .a，不重复 add_requires —— 本机重装 lexbor 需要 VS）。
    after_build 把 DLL 与 web/ 资产拷到 exe 旁边。
  - 关键坑：
    (a) 静态库链接顺序 —— `whaleui_link()` 必须挂在 **fs_ui**（引用 whaleui_* 的
        目标）上，挂 flashsight 会让 -lwhaleui-full 排在 fs_ui.lib 之前，全部符号
        未定义。
    (b) `ui_app.cpp` 一行未改，四个后端（wasm/null/whale/未来）共用同一套上层。
    (c) WhaleUI 没有"改写文本节点"的 API，所以适配层用**槽位下标**做 Node id，
        set_text 换 lexbor 文本节点而 id 不变。
    (d) 布尔属性（checked/disabled/selected）必须按**存在性**处理（"0" 要
        remove_attribute），否则 `:checked` 永远为真。
    (e) 空字符串 style 要 `style_remove_property`（不是写 `prop:`），否则
        diff 的"这轮不再设置"会留下陈旧内联样式。
  - 详细说明与验收清单：`doc-internal/ui-whaleui.md`（含 7 项已知缺口 A-G）。
  - 首次运行发现 WhaleUI 布局缺陷（界面只出标签条）：见 <39>。

<39>(SOLVED): WhaleUI 布局缺陷 —— flex 项的"主轴"尺寸用视口单位时塌成 1px。
  - 现象：桌面 UI 只画出 40px 顶部标签条，页面主体全空；布局 dump 显示应用根容器
    `.window`（`height:100vh;display:flex;flex-direction:column`）被算成 1280x1，
    所有 `flex:1` 后代跟着塌成 0。
  - 根因（只读定位，未改 WhaleUI）：
    `layout_flex` 解析 flex 项假设主轴尺寸走 `len_or_auto`（layout.cpp:3471）
    → `len_px`（layout.cpp:568）→ `len_px_impl(v, base, em, 0, 0)`：**视口宽高硬编码 0**，
    于是 `vh/vw` 全算成 0；随后被"避免 0 尺寸主轴项"的地板兜成 1
    （layout.cpp:3515 `if (main_size[i] < 1 && grow[i] <= 0) main_size[i] = 1`）。
    同一处百分比基数用 `inner_w`，导致 column 里 `height:50%` 按**宽度**解析
    （实测 640 = 1280 的 50%，应为 410）。
    带视口的 `len_or_auto_vp`（layout.cpp:595）已存在且元素自身尺寸已在用
    （layout.cpp:2704-2705），只有 flex 项这条路径没用；`est_node_height`
    （layout.cpp:4091）同病。
  - 复现与报告已交上游：`C:/aaa/WhaleUI/temp/incorrect1/`（`incorrect1.cpp` 15 个变体
    + `README.md`）。实测：单层 flex column + `height:100vh` 即触发；交叉轴不受影响；
    rem/px 正常；`min-height:100vh` 或 `height:100%` 可规避。
  - **2026-10-17 上游已修**（WhaleUI `e87979c` "fix-css-from-api-flex-main-axis-web-api-cpp-facade"）：
    flex 主轴尺寸改走 `len_or_auto_vp`，百分比基数按主轴取（column 用高度），
    `est_node_height` 同步修。重跑 `temp/incorrect1` 15 个变体**全部 ok**
    （app 1280x820、flex-col-vh 1280x820、flex-row-vw 1280x50、flex-col-pct 1280x410）。
  - FlashSight 侧动作：刷新 `3rdparty/whaleui/include/` 头文件、重建 WhaleUI 库、
    `xmake f --whaleui=y` 重编，桌面 UI **完整渲染且可交互**（标签切换、设置页双栏均正常）。
    不需要改应用代码。

<40>(SOLVED): 终端标签与"所有"解耦 + 空间利用 + 常规/上下文归位。
  - 关闭标签 ≠ 移除终端：`close_terminal_tab` 只把 tid 记进 `term_hidden`，
    不再从 `st_.terminals` 删除，所以「所有终端」照旧列出它（标"标签已关闭"），
    进程也不受影响；`reopen_terminal_tab` 从 `term_hidden` 和 `term_order` 里
    去掉再 `push_back`，因此重新打开是**追加到末尾**而不是按 tid 插回中间。
  - 新增 `AppState::term_order`（会话视图同步持久化）：`load_terminals` 保序
    同步（已知保序、新的追加、消失的丢弃），`ordered_terminals()` 统一供标签条
    与筛选后的概览使用，`terminal.output` 事件里也登记新 tid。
  - 输入框去掉「子代理」「终端」按钮（与侧栏「+」和底部终端抽屉重复）；
    `open_or_focus_side_tab` 先清 `side_collapsed`，程序化打开面板时侧栏必弹出。
  - 供应商页：表单改 `.pv-detail-cols` 两栏（`.pv-form` flex 1 1 340px +
    `.pv-models-panel` flex 1 1 260px），模型列表从折叠的「高级」里提出到右栏
    用 `.pv-model-row` 行渲染；模板网格 `auto-fill minmax(260px,1fr)` + 首字母
    徽标 `.pv-tpl-mark`。
  - 设置页空间利用：`.settings-body` 改 grid（`auto-fill minmax(400px,1fr)`），
    只有 `.field` 参与分栏，标题/提示/列表/自带多栏的页跨整行；
    max-width 860 → 1180。`ui.maxTabs`（默认 8，0 = 不限）新增到 settings_store。
  - 依据 rules/goal.md 重排设置：常规 = 界面显示（标签位置/会话名提示/标签上限）
    + 数据保留；新增「模型 · 上下文」承载短视图（history.recentTurns）、
    压缩（history.tokenBudget）、防过拟合（session.overfitEnabled）——这些决定
    **模型看到什么**，不是存储策略，所以归到模型下。
  - 修 CSS：`.field .hint` 改为 `flex 1 1 100%` 换行到控件下方。原先内联时在窄栏
    里与控件抢行并"赢"了，控件被压成 0 宽、提示直接画在 select 上。
  - 测试：161 用例 / 1560 断言通过（`uiapp_terminal_close_tab_keeps_process`
    改为断言终端仍在列表、新增 `uiapp_reopened_tab_appends`、
    `uiapp_settings_page` 加入 `context` 段）。
  - 注意：`.xmake` 里的 builddir 曾被改成 `build_check`，导致测试跑的是旧 exe
    （"改了没生效"）；`xmake f -o build` 复位。改动后务必确认 exe 时间戳。

<41>(SOLVED): 模型一等公民（逐模型设置 + 选择器 + 本地图标集）+ 工具条阈值重测。
  - **模型是独立对象**，不是供应商的属性：能力（视觉/音频/视频/PDF/结构化输出/
    工具调用）、上下文与最大输出长度、思考档位、失败重试都因模型而异。新增
    `ModelEntry` 字段（enabled/user_set/icon/thinking/retry + 多模态能力）、
    `ModelCapabilities` 扩到 8 项、`ThinkingConfig`/`RetryConfig` 两个结构。
  - **思考档位不是固定枚举**，取决于模型本身：`model_naming.cpp` 按家族粗匹配
    （o 系/gpt-5/Claude/Gemini 分 off-low-medium-high，DeepSeek-R1 类只有
    off/high，Qwen3 类 off/medium，普通对话模型**没有**档位 → UI 不渲染控件）。
    档位表在模型设置里可增删、可指定默认；删掉默认档时会自动改指第一个，
    避免存下一个悬空值。
  - **模型名粗匹配品牌图标**：新模型层出不穷，精确表必然过时，所以按家族子串
    判断（gpt-/o1/o3/o4→openai，llama→meta，glm→zhipu…）。认不出来返回空，
    UI 回退到中性圆点（不给错的品牌）。用户显式设的 icon 优先于匹配。
  - **图标集本地化**：Lobe Icons（`@lobehub/icons-static-svg` 1.95.1，MIT）取
    63 个品牌 + material-icon-theme（5.39.0，MIT）取 67 个文件类型图标，三个
    生成脚本 `tools/make_lobe_subset.py` / `make_file_icons.py` /
    `make_icon_css.py`。后者输出 `web/icons/icons.css`（**data URI**，三个后端
    通用，不用新增 DOM 图片 API）和 `src/ui/icon_tables_gen.hpp`（扩展名/文件名
    → 图标类，直接取自上游主题自己的映射，避免手写猜错）。
  - **坑：CSS 背景图不继承颜色**。monochrome 图标的 `currentColor` 会解析成黑色，
    暗色面板上直接看不见。所以 23 个单色图标生成浅/深两套（墨色写死），暗色用
    `[data-mode="dark"]` 与 `prefers-color-scheme` 双保险选中。
  - **坑：逗号拼接的完整规则不是合法 CSS**。`a{...}, b{...}` 里解析器只保留第一条、
    其余整条丢弃，暗色变体因此全部失效。必须每条规则独立一行。
  - 供应商页右栏改成「上=可滚动模型列表（启用圆点/测试/删除/新增，点开编辑）
    + 下=该模型高级设置」。只有列表滚动：同一面板两个滚动区会分不清滚轮打给谁。
  - 会话输入框的模型 `<select>` 换成**真选择器**：左供应商 / 右模型，>20 个时按
    首字母排序 + 分组（分组只有排序后才有意义），带搜索（内存过滤，不来回请求）。
  - **工具条阈值重测**：子代理/终端去掉 + 模型 select 换按钮后，标签行只需
    ~580px，而 940px 的旧阈值让"明显放得下"时仍是图标。新阈值 600px，并且
    `container-type: inline-size` 用的是 composer 的内容盒（1280 窗口下 608px），
    所以标签现在能正常展开。阈值必须跟着行内容重测，不能凭猜。
  - 新增 FsApi：`model.list/save/remove/setEnabled/test/allEnabled`（持久化到设置
    `provider.models`，形状 `{providerId: [ModelEntry]}`；删除必须整份替换写回，
    逐键合并表达不了"删除"）。启动时 `load_user_models` 恢复。
  - `model_naming.cpp` 单独成文件：后端和 wasm UI 都要用它（UI 要在用户还没保存
    新模型时预览它的档位），而 wasm 构建不编译 provider 层，所以它只依赖
    `<string>/<vector>` 并同时进 `ui_shared` 和 `fs_core`。
  - 测试：183 用例 / 1682 断言（新增 6 个模型表用例 + 11 个 UI 用例）。
  - **坑（重要）**：`.xmake` 的 builddir 又漂到 `build_check` 一次；另外有两次
    "改了没生效"是因为 exe 被运行中的服务器占用导致链接失败（ld: cannot open
    output file），必须先 taskkill。排查时先看 exe 时间戳。

<42>(SOLVED): 弹出层归属、文件预览独立标签页、访问栈、顶层"+"新建页、项目卡片。
  - **模型选择器曾经"粘住"**：它被放在 `overlay-layer` 里，而那一层是
    `pointer-events: none` —— 背景遮罩收不到点击，所以点哪儿都关不掉，切标签
    也不会消失。改成和 "/" 菜单一样**锚在输入框上方**（`position: absolute;
    right:0; bottom: calc(100% + 8px)`），遮罩进 composer，点外部/✕/选模型都能关。
    另外 `switch_tab` 里统一清掉所有临时弹层（选择器/skill 菜单/侧栏"+"页）：
    弹层属于它打开时所在的那个标签页。
  - **点文件 = 侧栏新开"预览"标签页**（不再内联在文件树下面）。原来内联的
    小 div 把文件树挤出视野，没法边读边浏览。预览页按**文件名**命名标签
    （一排"预览"分不清谁是谁），同一文件重复打开是聚焦而不是叠标签。
  - **访问栈**：`AppState::tab_history` / `side_history`（最近访问在末尾）。关掉
    当前标签页时回退到"上一个还在的"，栈空了才回 home/第一个面板；切换时记录
    来处，并且会跳过已经关掉的条目。以前关标签固定跳 home，看起来就是"乱跳"。
  - **顶层 "+" 打开"新建"标签页**（`kNewTabId`），内容就是主页·项目那一套卡片：
    点会话即打开、点"新建会话"即建。以前是"直接给第一个项目开会话"——既猜错
    了项目也不给选择。同一时刻只保留一个"新建"页。
  - **项目卡片**：会话行带**状态点**（有回合在跑就是绿的，来自
    `AgentHost::is_running`，不是落盘的历史）+ **相对时间**（"3 分钟前"；>30 天
    给日期），按最近活动排序；hover 显示 ✕ 删除（先弹确认框，删除会连
    patches/terminals 一起删，不可恢复）。左列改为顶部对齐、按钮改成图标+文字
    （"打开"/"移出"，带 tooltip），压缩空白。
  - 新增后端：`SessionFileStore::remove`、`SessionManager::delete_session`
    （先释放锁再删目录，否则残留锁记录会让同名 sid 被误判占用）、
    `SessionManager::full_mtime_ms`（用 `duration_cast` 而不是 `.count()`：
    file_time_type 的周期是平台定义的，Windows 上是 100ns tick，直接乘 1000 会
    差四个数量级）、`AgentHost::is_running`、`FsApi::session_delete`，`session.list`
    现在补 `updatedAt`/`running`。
  - **坑（重要）**：`panel_tabs` 的标签**没有 key**。diff 按位置匹配，关掉中间一个
    面板会让后面的节点整体左移、处理函数跟着槽位走——于是"关一个面板"触发了
    下一个槽位的动作（实际表现是侧栏"+"页自己弹出来了）。给每个面板标签和两个
    "+" 按钮都加了 key。顶栏 `tab_strip` 早就有 key，侧栏漏了。
  - 测试：195 用例 / 1732 断言（新增 8 个：访问栈回退/跳过死条目、切标签关弹层、
    侧栏访问栈、预览标签页复用与回退、顶层"+"页、卡片会话行、删除确认、
    相对时间格式、面板标签 keyed）。

<43>(SOLVED): 设置页按 temp/settings1.md 全面返工 + 设置/新建改为非标签页。
  - **设置与新建不再是标签页**：`AppState::modal_page`（""/"settings"/"new"）盖在
    当前标签页之上，点开即显示、关掉即销毁、**不进访问栈**。否则"关掉一个会话
    标签"会回退到设置页。`switch_tab` 里清空它（导航走了就是不再需要）。设置页
    因此自带标题栏 + ✕；`open_new_tab` 改为 `open_modal_page("new")`。
  - **标签位置真正实现**：`Appearance::tabs` → `data-tabs="left"`（wasm/whale 两个
    适配器都写根元素），CSS `:root[data-tabs="left"] .window{flex-direction:row}` +
    `.tabs{flex-direction:column;width:190px;height:100vh}`。切设置立即生效。
  - **标签条可滚动**：删掉 `ui.maxTabs`（上限只会藏起用户已经打开的标签），
    `.tabs{overflow-x:auto}` 且 tab `flex:none`。
  - 常规：界面显示（标签位置/会话名提示）+ 会话行为（过拟合检测，从原"上下文"
    移来）+ 数据保留。
  - 外观：只留界面语言（思考语言移到模型偏好——它是给模型的指令，不是界面外观）。
  - **删除整个"上下文"段**：短视图轮数/token 预算/防过拟合三项按反馈取消
    （防过拟合改名"过拟合检测"并入常规）。
  - 会话默认：模型改用**共享模型选择器组件**；思考档位取自该模型粗匹配的档位表；
    文件权限/自动模式改为"图标+标题+说明"的 `choice_list`（原来只有 approve/
    permissive/full 三个英文词，没人看得懂），并加"记忆上次"；删除 Enter 行为
    （无实际效果且与快捷键页冲突）。
  - 模型偏好：输出语言（不指定/中文/English，默认不指定）+ 思考语言 + 自动压缩
    阈值（60/70/80/90/关闭）+ 摘要模型（模型选择器）。去掉"子代理默认模型"
    （子代理页每个代理自己选）。
  - 子代理：每行改用共享模型选择器（原来是从 provider_models 拼的下拉，数据还是
    假的）。
  - **MCP 与 Skill 拆成两页**：各自有"自动探测/手动导入/本地搜索 + 卡片列表
    （名称/路径/简介/启用）"，空列表只显示"暂无"提示，不再在下面挂一个表单。
    Skill 页明确写出"Skill 是唯一可以由你输入 /名称 调用的命令"。
    后端新增 `skill.import/create/setEnabled/remove`（`SkillRegistry` 加
    `import_dir` 读 SKILL.md frontmatter、`add/set_enabled/remove/user_json/
    load_user`），持久化到 `skills.entries`；内置 skill 删不掉只能停用。
  - 长期记忆：改成**便签卡片**（模型自己写的提醒），不再是用户维护的 KV 表。
  - 浏览器控制：驱动路径加"选文件"图标按钮；删掉"加载 skill 说明"（加载 skill
    是模型按需决定的事，不是用户动作）。
  - 网络：代理改三态（跟随系统/自定义/关闭），只有"自定义"才显示主机与端口。
  - 终端：探测结果做成可选列表（显示解析到的路径，两个都叫 pwsh 的安装才分得清）
    + "重新探测"。
  - 环境：合并成一张 CLI 工具表 + "自动探测"（`ShellDetector::detect_cli_tools_json`
    在 PATH 上找 100+ 常见工具，只返回找到的）+ 每行/新增行都能"选文件"。
  - 供应商：进入页面自动选中第一个供应商；模板徽标改用本地 Lobe Icons（
    `provider_brand_for_template` 映射模板 id → 品牌键）；模型面板加"全选/全不选"
    与来源标记（内置预置 ≠ API）；能力开关改两列网格、去掉"工具调用"（默认都支持）；
    上下文长度改为必填并在保存时校验；思考档位改成**逗号分隔单行输入** + 默认档
    下拉（off/low/high/xhigh/max 等预置中文翻译，未知档位原样显示）。
  - **修"获取模型列表是假的"**：`provider_list_models` 拿到 API 结果后没存进
    ModelTable，于是 `model.list`（模型面板读的那条路径）每次都回退到内置预置，
    看起来永远只有那几个固定模型。现在会 `set_api_models` 存下来。
  - 新增 `dialog.pickFile`（`pick_file_dialog`：同一个 IFileDialog，不带
    FOS_PICKFOLDERS）+ `environment.detectCliTools`。

  - 补充（同一批收尾）：
    * **opencode 的会话亲和头不再是特殊开关**：模板预填一条普通自定义头
      `x-opencode-session: {{SESSION}}`，`ProviderClient` 在发请求前把
      `{{SESSION}}` 换成会话 id（三种形态都替换）。Azure 的 api-key 走同一条路径
      ——"某种供应商才有的隐藏开关"这个概念被彻底去掉了。
    * 高级里新增**请求体覆盖（JSON）**：与默认请求体浅合并，供应商特有的字段
      （extra_body 之类）不用改代码；保存时校验必须是 JSON 对象。
    * 修一处历史遗留：`agent_loop.cpp` 里一条注释被写成了占位符文本，已改回
      通顺描述。
  - 测试 197 用例 / 1726 断言。

<44>(SOLVED): WhaleUI 二次验收（0243808）—— 布局修好后跑真实 UI，发现 4 个新缺陷。
  - 上游 `e87979c` 修掉 flex 主轴视口单位塌 1px 后，重新拉头文件、重建库、
    `--whaleui=y` 重编，桌面 UI **完整渲染并可交互**（主页/探索/设置、标签切换）。
  - 逐屏对比浏览器版发现 4 处不对，已写成复现交上游：WhaleUI `temp/incorrect2/`
    （`incorrect2.cpp` + `README.md`，用例直接加载 `web/style.css`）：
    * H. flex 项带 `overflow:hidden` 就塌成 1px（1x342 vs 228x18）。触发条件是
      `overflow != visible`，与 `min-width`、样式表都无关。根因：`layout_flex`
      把"可收缩到 0"当成"初始尺寸 0"（`layout.cpp:3550` 地板 + `:3631` 的 1px 兜底）。
      影响：探索页所有描述不可见。
    * I. `<button>` 的显式 `height` 被 UA `min-height` 压掉（34x32 vs 34x19）。
      根因：`theme_css.cpp:179` 的 `min-height: var(--btn-h)`（fluent 32px），
      而 `min-height` 恒优先于 `height`；浏览器 UA 没这条。影响：开关 knob 脱轨、
      所有按钮偏高。
    * J. grid `repeat(auto-fill, minmax(400px,1fr))` 列宽算成 122px（应 ~510）：
      `minmax` 下限未参与列数、`1fr` 没分剩余空间。连带 label 折行、toggle 只剩 8px。
      影响：设置页挤成一列。对照 `display:block` 正常。
    * K. flex 行含 `select` 时可用宽度少 52px（866 vs 918，换成 div 就正常）。
  - 关键方法：复现用例**必须加载真实 style.css**——最初用简化 HTML 测不出来，
    换成真实样式表 + 真实 DOM 结构才复现（grid 缺陷尤其如此）。
  - 详细清单与规避见 `doc-internal/ui-whaleui.md` §5（H/I/J/K）。

<45>(SOLVED): 会话页/主页细节 + 设置整窗 + 图表动画 + 方向感知切换。
  - **上下文水位贴边**：删掉输入框下方那条横贯整行的 3px 进度条（用户说"太丑"），
    改成贴在模型选择按钮**底边**的 2px 细线（`.model-btn-wrap` + `.model-level`，
    absolute 定位，左右各缩进 3px，`pointer-events:none`）。低水位是 55% 透明度的
    中性色，60%+ 变琥珀、85%+ 变红——平时几乎看不见，只在需要时提示。侧栏用量页
    仍保留完整数字。
  - **自动挡 ↔ 批准模式互换**：工具栏左半顺序改为 文件/命令/规划/**自动**/**批准**。
    理由是"先决定谁来做（人/AI），再决定它能自己动手到什么程度"；下拉放在最外侧
    也让它朝外展开。
  - **不再预置 GPT 模型**：`ModelTable::list_models` 删掉"空列表就回退预置表"的
    分支——一个从没连过 API 的供应商下面列着 gpt-4o/claude 会让人以为已经配好，
    实际一个都调不通。`provider.listModels` 失败时也只返回本地已知模型（用户自定义
    + 上次成功获取的），level 从 `preset` 改成 `local`。预置表仍用于 `resolve()`
    （用户手写一个已知 id 时补能力标签）。
  - **默认模型不再写死**：`SessionView::model` / `AppState::model` 的默认值从
    `gpt-4o-mini` 改成空。空值 → 按钮显示"选择模型"，若设置里配了
    `session.default.model` 则用它。同时 `session.send` 现在把当前 model/thinking
    一起带上（后端先 `set_model`/`set_thinking` 再发），修掉"换了模型立刻发送仍用
    旧模型"的竞态。
  - **设置整窗覆盖**：`build_shell` 在 `modal_page=="settings"` 时**不渲染标签条**，
    直接把设置页作为 window 的唯一子节点。设置里改的是整窗的东西（外观/标签位置/
    语言），标签栏在旁边既没用又诱导用户以为能切走。
  - **用量统计动画 + 真实按天柱状图**：后端一直在返回 `byDay` 但没人画。新增
    `.usage-byday` 柱状图：按时间窗补齐空白日（0 高度柱也占位，否则时间轴会骗人），
    按峰值归一化，`grow-y` 从 0 长到 `--grow`，逐根 16ms 错开；窗口/供应商变化时
    整个 plot 重新 key，动画重播。日期轴用同一套 flex 规则（柱子与标签逐列对齐）。
    按模型的 `ub-fill` 改成 `grow-x` 从左铺开 + 行错开进场；总计四项依次淡入；
    左侧 scope 列表用 `anim-list`。柱子最大宽度 72px 且整组居中（7 天窗口不会变成
    1000px 里七根细棍）。
  - **方向感知切页**：`switch_tab(id, dir)`。默认按 `tab_history` 判定——目标正好是
    栈顶（刚离开的那一页）就是"后退"，播 `anim-slide-right`（从左进）；其余都是
    前进，播 `anim-slide-left`。关闭标签时栈已 pop，显式传 `dir=-1`，所以关标签
    永远是后退动画。`open_modal_page/close_modal_page` 清掉方向，modal 不参与。
  - **两个图表 bug（自测发现）**：
    * `IDom::set_style` 在 wasm 后端用 `style[key] = value`，**CSS 自定义属性
      （`--grow`）用这种方式赋不进去**（CSSStyleDeclaration 不暴露 `--x`），于是
      柱子宽度/高度永远是 0，动画等于没跑。改成检测 `--` 前缀走 `setProperty()`。
      这个坑对以后所有自定义属性都适用。
    * 用量行里 `in/out` 全是 0（供应商没回报 usage）时，按天图会是一排 0 高度柱。
      聚合层 `UsageRow` 新增 `calls`，`usage.query` 的 byDay/byProviderModel 带上
      它；前端在 token 全为 0 时改画"调用次数"并在标题里说明。
  - 测试 201 用例 / 1822 断言（新增：贴边水位、自动挡顺序、方向感知、设置覆盖
    标签栏、按天图表、聚合 calls；`model_table_list_merges_and_falls_back` 改为
    断言空列表）。

<46>(SOLVED): 框架双 view + 左栏模式 + 拖拽排序 + 细节修正。
  - **双 view 切换（框架层）**：页面切换时旧页面不再"立刻消失 + 新页面带着两个
    动画进来"。`VNode` 新增 `exit_anim`；`patch` 里被丢弃的子树不立即 `remove`，
    而是加上 `.page-exit`/`.exiting` + `anim-exit-*` 播完再删（`sweep_exits`，
    由 render 与 250ms pump 驱动）。于是旧页面在退、新页面在进，是两棵独立 DOM
    各自播自己的动画。离场前会剥掉 `anim-*` 类，避免同一元素上同时跑进场和离场
    （这正是"看着奇怪"的原因）。方向在 `render()` 里按本次移动盖到旧页面上
    （创建时烘焙的方向是错的：后退时旧页面也该往反方向退）。`IDom` 新增
    `supports_exit_animations()`：NullDom 立即删除（否则单测里关掉的页面还留在
    dump 里），wasm/Whale 返回 true。
  - **大部分组件移出/消失都有动画**：行/卡片/popover 被丢弃时统一淡出
    （`has_visual_class` 白名单），不再只有进场。
  - **侧栏面板切换有动画**：`.side-body` 按 active_side 做 key + anim_key，切面板
    时整块重建并进场。
  - **标签拖拽排序**：`IDom::bind_tab_drag` + `fs_dom_tab_drag`（拖起/悬停/落下
    三段，报告 "from\tto"），`UiApp::move_tab` 移动标签并把 `ui.tabOrder` 写回
    设置；启动时 `apply_saved_tab_order` 恢复。主页固定第一位。
  - **项目卡片 hover 不再改高度**：行高固定 32px，重命名/删除按钮改成绝对定位
    覆盖（原来 display 切换会撑高一行），`:hover` 只改颜色。
  - **用量统计初始加载供应商**：进入用量页（点击或 home_tab=2 直接渲染）都会
    触发 `provider.listProviders`，左侧范围列表不再空白。
  - **左侧栏模式重做**：`ui.tabs=left` 时不再是"竖过来的标签条"，而是项目导航
    （`build_nav_rail`）：按项目列出全部会话、宽度可拖（`bind_nav_splitter`，
    独立回调）、可最小化成 34px 窄条。左右两个分隔条各自独立——拖导航只改
    导航↔内容，拖右侧栏只改内容↔右侧栏。设置与外观在启动时加载
    （`mount()` 调 `load_settings_section("")`），保存的左栏/配色/字体不再要
    等打开设置页才生效。
  - **设置文案与布局**：删掉长期记忆的空状态文字（上面已有 hint）、删掉
    "{{SESSION}} 会被替换…" 与 "与默认请求体浅合并…" 两句；请求体覆盖改成
    标题在上、输入框在下（JSON 需要宽度），`.pv-body-field`。
  - **"记忆上次"可选了**：原来把 `remember` 写进 `session.default.autoMode`
    （布尔）并读同一个键，选择永远匹配不上。改成独立标记
    `session.default.autoModeRemember` / `filePermRemember`；`SessionManager`
    在 set_auto_mode/set_permission 时记下 `session.last.*`，新会话在 remember
    打开时沿用它。
  - **后端接线核对**：`session.send` 带 model/thinking（先落盘再发）；
    `provider.listModels` 失败返回本地已知模型；`usage.query` 的 byDay/
    byProviderModel 带 `calls`；`project.list` 带 sessionCount；`session.rename/
    delete`、`session.setAutoMode/setPermission/setModel/setThinking`、
    `settings.set`、`skill.*`、`model.*` 均已注册。
  - 测试 204 用例 / 1873 断言（新增：标签拖拽排序、左栏导航、双 view 页面槽）。

<47>(SOLVED): 设置/新建改为真正的覆盖层 + 左栏启动即生效。
  - `build_shell` 拆出 `build_shell_into(H&)`：正常 shell 始终挂载，设置/新建作为
    最上层 `.modal-layer`（absolute inset 0，z-index 30）叠上去。关闭时整层播
    `anim-exit-fade` 后再删，所以"关设置"也有退场动画，而不是整个 window 换树。
    这也解释了之前设置关闭没有动画：模态页替换的是 page-slot，框架看到的是容器
    被换掉，根本轮不到页面的 exit_anim。
  - 离场方向改在 `render()` 里按"这一次移动"盖到旧页面上（创建时烘焙的方向在
    后退时是错的）。
  - `mount()` 加载设置：保存的 `ui.tabs=left` / 配色 / 字体在启动时就生效，不再
    等打开设置页。
  - 文件权限的"记忆上次"也用独立标记（`session.default.filePermRemember`），
    新会话按各自开关分别沿用 `session.last.autoMode` / `session.last.filePerm`。
  - 204 用例 / 1878 断言。

<48>(SOLVED): 动画节奏、权限语义、新建页位置、左栏导航、i18n 补齐、拖拽反馈。
  - **动画错开（双 view 的节奏）**：离场与入场同时开始会让两页交错，看着乱。
    现在入场页加 `.page-stagger`（`animation-delay: .18s` + `backwards`，等待
    期间保持不可见），旧页 `.22s` 退到 ~80% 时新页才开始进。页面入场也从
    `translateY(10px)` 改成原地 `scale(.996)`——"从最底下升上来"不是切页的语义。
  - **新建页不再覆盖标签栏**：只有设置是整窗 `modal-layer`；"新建"回到 page-slot
    里（标签栏照常可见可点）。左栏模式下没有标签条，所以导航顶部加了"新建"
    （打开同一个新建页）与"主页"入口。
  - **文件权限语义（后端）**：`AgentLoop::needs_approval` 按档位判定——
    approve=所有写/删/网络/终端都问；permissive=项目内直接放行，项目外路径/
    终端/网络要问；full=不问。`ToolContext::approved` 让"批准过的这一次"真的
    放行（否则问完还拒绝）。`file_tools` 的写门也按"是否逃出项目"判定，
    `file.ls` 走 `resolve_by_perm`。`AgentHost::loop_for` 现在从 full.json 的
    meta 读 cwd/pid（之前 Loop 的 cwd 是空的，项目内外根本无从判断）。
    三档图标也分开：逐次批准=手、宽松=文件夹、完全放行=闪电。
  - **i18n 补齐**：新增 ~295 条英文条目，并把 `ui_app.cpp` 里 200+ 处直接写死的
    中文（`.text()`/`button`/`field`/`hint`/`empty_state`/`title`/`toast`）包进
    `tr()`；`ui_components.cpp` 也接上 i18n（复制/本轮要点）；相对时间、会话
    标题前缀、用量范围标签、主页标题按钮都走 tr。扫描脚本确认剩余未翻译项只有
    动态拼接的片段。
  - **标签拖动反馈**：拖动超过 4px 才进入拖拽（避免把点击当拖动），被拖标签
    变淡、跟随光标的浮动副本、落点标签高亮；拖动结束吞掉随后的 click。
  - **构建流程坑**：`emcc` 不在 PATH 时 `fs_ui_wasm` 会被静默跳过，浏览器里
    一直是旧 UI。构建前需 `set PATH=C:\Program Files\emscripten\emscripten;%PATH%`。
  - 205 用例 / 1888 断言（新增 `loop_permission_levels`）。

<49>(SOLVED): 动画节奏再调、Skill/MCP 归位、搜索与状态、选择器定位。
  - **Skill 页**：删掉"Skill 是唯一可以由你输入 /名称 调用的命令"（没有信息量）。
    内置的 browser-control / lsp 其实是 MCP（模型自主调用，用户敲不了 /lsp），
    现在带 `kind="mcp"` 标记并显示在 MCP 页（带"内置"徽标），Skill 页只剩真正
    可 /调用的条目。
  - **动画**：离场 .26s、入场延迟 .2s（旧页约 80% 时新页才进），滑动时长从
    .16s 提到 .22s——之前"旧的刚走新的就到"是因为两步几乎同时结束。主页三个
    子页（探索/项目/用量）切换改用 `anim-fade`（原来套的是行入场动画，会
    translateY，看起来像"从底部弹上来"）；tab 切页也是同一处根因。
  - **左栏**：主页底部那条 footer 在左栏模式隐藏（栏里已有打开文件夹/设置），
    同时把"新建项目"补进左栏底部。抽搐的原因找到了：`patch_attrs` 每次渲染都
    无条件重写 inline style，把拖拽时直接写在 DOM 上的宽度覆盖回 state 里的旧
    值（4Hz 抖动）；现在只写"值真的变了"的样式。
  - **侧栏标签拖拽排序**：与顶栏同一套（4px 阈值、浮动副本、落点高亮），
    `move_side_tab` 重排 `side_tabs`。
  - **会话状态点**：蓝=已完成、红=上次出错、黄=等待确认、转圈=运行中、空心=空闲。
    后端 `session.list` 现在带 `loopState` 与 `lastError`（`AgentHost::loop_state`
    + 读 full.json 最后一轮），项目卡片、左栏、搜索页共用同一个 `session_dot`。
  - **搜索**：新增全局搜索页（主页标题栏最右的放大镜、左栏搜索图标、新建页的
    搜索框都指向它），跨项目搜会话标题并显示所属项目；新建页输入关键词时直接
    切换成搜索结果。左栏另有本地筛选框 + 最近/名称/状态排序。
  - **模型选择器**：新增 `IDom::place_popup` / `bind_outside_click`。定位改在
    弹层自己的 containing block 里计算（之前用视口坐标，弹层跑到按钮半屏外），
    上方放不下时翻到下方；点击弹层外（及其触发按钮外）关闭，不再依赖全屏
    backdrop（在设置里那个 backdrop 会盖住字段本身）。
  - 210 用例 / 1913 断言（新增搜索页、状态点、左栏筛选排序、侧栏排序、
    选择器关闭）。

<50>(SOLVED): 动画时序根因、侧栏/主页搜索归位、演示会话。
  - **反向动画"淡入没跟着跑"**：离场节点继承了自己入场时的 `animation-delay`
    （.2s stagger），于是旧页先僵住 .2s 才开始退；新页也在等，两边叠成"卡一下
    再跳"。现在离场时显式写 `animation-delay:0s` + `fill:forwards`。
  - **"有概率还是很急促"**：stagger 之前是加 class，而 `render()` 里那次
    "按本次移动盖章"只在 diff 前加一次；任何一次过渡中的重渲染都会重建 VNode
    并丢掉这个 class，入场动画于是从延迟态跳到终点。改成**内联样式**
    （`animation-delay` / `animation-fill-mode`），每次渲染都会重新写上，下一次
    切换直接覆盖，既不丢也不泄漏。
  - **离场节点清理**：原来靠 render pump 的 `sweep_exits`，后台标签页的定时器被
    浏览器节流时节点会滞留数秒（表现是"旧的还在"）。`IDom` 新增 `schedule()`
    （wasm 用 `setTimeout` + `fs_ui_timeout`，Whale 用 `whaleui_set_timeout`，
    Null 队列化供测试触发），离场后按动画时长自己删。`sweep_exits` 保留为空实现。
  - **侧栏搜索**：左栏那个输入框太挤（会把会话标题压没），改成放大镜图标跳到
    全局搜索页；排序（最近/名称/状态）留在栏里。
  - **主页搜索右对齐**：`.home-head` 改成 `position:relative`，搜索按钮
    `position:absolute; right:14px`。之前作为 flex 兄弟节点跟着页签文字宽度浮动。
  - **会话设置回填**：`load_session` 现在读 `sessionSettings`（model / thinking /
    filePerm / autoMode）并写进视图与当前扁平状态——之前打开已有会话时输入框显示
    "选择模型"，发送还会用错模型。顺带修 `switch_tab` 用"有没有 stash 过 view"
    判断是否拉历史的 bug（离开时会 stash 默认 view，导致回到一个从没加载过的
    会话时跳过 `load_session`），现在每次进入都重读，磁盘是事实来源。
  - **思考过程**：`ChatMsg` 增加 `thinking` / `thinking_open`，渲染成回答上方的
    折叠块（默认收起，点标题展开）。后端 turn 里的 `thinking`/`reasoning` 字段
    会带进消息。
  - **工具块按索引折叠**：一个消息里可能有十几个块，之前点击任何一块都切换第一块。
    现在事件值携带 `<index>\t<action>`。
  - **演示会话**：`temp/make_demo_session.py` 生成 `s_d00d000001`（projA），9 轮、
    7 段思考、19 个工具块（read/diffWrite/write/terminal/ask/todo/subagent）、
    5 种语言代码块（cpp/python/rust/ts/go）、1 次失败重试、4 条 systemOps
    （两次 setModel + setThinking + setPermission）、5 步计划。
  - 214 用例 / 1928 断言（新增：会话设置回填、思考折叠、工具块独立折叠、
    自动挡状态）。

<51>(SOLVED): 框架重排 bug（一连串"抽搐/重建/滚动丢失"的总根因）+ 会话页信息设计。
  - **框架根因**：`patch_children` 的 reorder 循环对**每一个**子节点都调用
    `insert_before`，即使它已经在正确位置。DOM 的 insertBefore 会把节点先摘下来
    再插回去，于是：滚动容器位置被重置、CSS 动画重播、节点被"重建"。
    这一条同时解释了"点规划把 chat history 重建了""滚动位置没保存""侧栏动画
    先出现在垂直 50% 再跳位""点 diffWrite 反复切换侧栏"。修复：先判断存活子节点
    是否已是目标顺序的子序列，是则只插入**新建**的节点（append 场景完全不碰
    已有节点）；`WasmDom::insert_before` 也加了"已在位则 no-op"的守卫。
  - **规划面板**：从 `.session-center` 里的 absolute 浮层改成**独立一列**
    （`.plan-col`，258px，chat 与侧栏之间）。它不再是 chat 布局的一部分，开关
    只改变列宽，历史节点与滚动位置都不动。实测：打开后 `.history`/`.history-inner`
    的 DOM 身份不变，9 条消息节点全部保留。
  - **滚动位置**：`hist_scroll` 进 SessionView（每会话独立）。离开标签时读取、
    返回时写回；首次进入（-1）滚到**底部**。历史加载完成后再滚一次（此前滚的
    是空页面）。同时去掉 `.history` 的 `scroll-behavior: smooth`——它让程序化
    `scrollTop` 变成动画，既读不回来也会和恢复互相打架。
  - **宽度**：`.history-inner` 的 `max-width: 50%` 改成
    `min(50%, calc(100% - 20px))`，窄列不再被 50% 卡死。
  - **工作过程**：思考 + 工具调用合并成一个折叠组（`.work-block`），默认收起。
    折叠时显示"N 步 · 完成/有失败 · 时长 · 结束时刻"，展开后分"思考/工具调用"
    两段。`ChatMsg` 增加 `work_open` / `started_at` / `ended_at`；
    `SessionManager::append_turn` 落库时补 `ts` 与 `startedAt`（结束时间与时长
    只能从数据里来，不能靠 mtime 猜）。
  - **代码块**：`.code-wrap` = 语言标签 + 行号槽 + 复制按钮 + `<pre>`。行号是真
    元素（CSS counter 会数到折行后的视觉行）。复制走新的 `IDom::copy_text`
    （浏览器 `navigator.clipboard` + textarea 回退；Whale 暂不支持）。
  - **子代理历史**：`SubagentRow` 增加 transcript / started_at / ended_at /
    model / summary / open；`subagent.list` 本来就带 `full`，现在解析并渲染成
    可展开的对话记录（角色 + 内容），卡片头显示模型、已运行/耗时与结束时刻。
  - **终端**：去掉每行 `.ts-row` 的入场动画（一行终端会"往上抖"，40 行会播 40 次
    动画），改为整块 `.ts-lines` 淡入；同时裁掉网格末尾的空行——新终端只有一行
    输出却有 23 个空行，撑出滚动条。
  - 219 用例 / 1955 断言（新增工作过程分组/时长、代码块行号与复制、规划独立列、
    子代理历史与时长、滚动状态）。

<52>(SOLVED): 语法高亮（不用 re2c，一套手写扫描器 + 语言数据表）
  - **为什么不用 re2c**：一个 C++ lexer 的 DFA 表约 30-60KB（每个状态 256 项），
    几十种语言就是 2-4MB 二进制，只为"上色"；而且每种语言都要维护 .re 源文件、
    加构建规则、保持同步。这里改成**一个手写扫描器 + 每语言一份 LangSpec 数据**
    （注释标记、字符串形态、是否区分大小写、关键字表），加语言 = 表里加一行，
    没有代码生成、没有新状态机。45+ 种语言全部源码约 40KB。
  - **结构**：`src/common/fs_highlight.hpp/.cpp`。`Syntax` 分五个专用扫描器
    （markup / css / json / markdown / diff）+ 通用扫描器；通用扫描器的行为由
    `Flag` 位掩码驱动（块注释、嵌套注释、C 预处理、@注解、三引号、raw string、
    `$变量`、反引号、Rust `#[attr]`）。关键字/类型用**懒建**的 unordered_set，
    没人打开的语言一个字节都不花。
  - **输出是字符串不是 DOM 树**：`to_html()` 直接产出
    `<span class="hl-kw">…</span>`，配合新增的 `VNode::html` + `IDom::set_html`
    （wasm innerHTML / Whale `whaleui_dom_set_inner_html` / Null 存原文供断言）。
    200 行代码块若按 token 建节点是上千个，而且流式输出时 UI 每秒重绘数次，
    每帧重新 diff 上千节点纯属浪费。`to_html_cached` 按 (语言, FNV-1a 内容哈希)
    做 96 条 FIFO 缓存。
  - **正确性靠 round-trip**：所有 span 文本拼接必须逐字节还原输入——高亮是显示层，
    唯一不能做的事就是改变代码内容。测试覆盖未闭合字符串、孤立单引号、CRLF、
    中文注释、非法代码、所有语言的交叉组合。
  - **UI 接入**：`markdown_blocks` 的 `<pre>` 用 innerHTML 写高亮结果；
    文件预览按**扩展名**猜语言（`lang_for_path`：Makefile/Dockerfile/CMakeLists
    走整名匹配），未知扩展名退回纯文本。`ui.codeHighlight` 开关走 CSS
    （`[data-code-highlight="0"] .hl-* { color: inherit }`），切换不需要重绘。
  - **配色**：一套（代码块在浅色模式下也是深色底 `--code-bg`，再做浅色变体只会错），
    12 个 token 类只改颜色，不加粗不变底色，长代码块才不吵。
  - 234 用例 / 2370 断言（新增 14 个高亮用例 + 代码块/预览接入断言）。详见 current.md <52>。


<53>(SOLVED): 历史不重算（业务缓存 + 框架短路）+ Git 面板接真后端 + 用量面板去假数据
  - **为什么历史不能每帧重算**：`build_session` 每次 render 都把整段历史重新解析
    （markdown → VNode），流式输出时每秒重绘数次；几百轮会话等于每秒把整份历史
    重新解析、再 diff 一遍自己。两处修复：
    1) **业务级**：`ChatMsg::rev` 版本号 + `msg_nodes_` 缓存（key = sid+index）。
       未变化的消息交回**同一个 VNode 指针**，只有真正变的那条被重建。校验用
       rev + 轻量签名（长度/折叠位，不做字符串比较）——因为状态也会被 handler
       和测试直接改，只靠调用方自觉 bump 版本号，漏一处就会静默显示旧内容。
       已固化的一轮 rev 永远不变，二次打开会话的代价降到一次 map 查找。
    2) **框架级**：`patch_node` 增加同指针短路。缓存复用同一对象时，diff 否则仍会
       遍历整棵子树和自己比字符串——正是缓存要省掉的那笔开销。
    流式 delta / tool.update / turn.done / 折叠点击都会 bump rev；`turn.done` 还会
    让该会话的用量重新拉取（这一轮花了 token）。
    测试 `uiapp_history_is_not_rebuilt` 断言：追加一条只重建那一条、空渲染零重建、
    改一条只重建那一条。
  - **Git 后端**：新增 `src/common/fs_git.hpp/.cpp`，走 git CLI（零依赖，不引
    libgit2）。`git -C <repo> <args>` 用参数向量 + 平台转义，路径**永远**不能
    被当成选项（`--force`、`../x` 直接拒绝，有测试）。stderr 单独写临时文件，
    不并进 stdout——否则 git 的 "LF will be replaced by CRLF" 警告会污染 diff
    解析。状态用 `--porcelain=v1 -z --branch` 解析（含 ahead/behind、游离 HEAD、
    merge/rebase 进行中标记）。10 个方法：status/diff/showHead/log/branches +
    stage/unstage/stageAll/commit/discard，`FsApi::git_*` 全部按 pid 解析路径，
    UI 不碰仓库路径。
  - **两个真 bug**（都是浏览器实测出来的）：
    * 未跟踪文件点开显示"没有差异"——`git diff` 比的是工作区与**索引**，未跟踪
      文件没有索引项。改为渲染成整文件 `+` 的 diff（就是暂存会记录的内容）。
    * "全部暂存"用的是 `git add -u`，不含未跟踪文件，但面板把它们列出来了。
      改 `-A`，并加测试断言列出的每一项都被暂存。
  - **侧栏 Git 面板**：分支/↑↓/进行中操作、已暂存与未暂存两组、单文件暂存/取消/
    撤销、diff 用 `diff` 语言高亮、仅在暂存非空时出现提交框（提交空内容是个错误，
    摆着按钮只会诱发那次点击）。
  - **用量面板去假数据**：原来写死 `42%` / `¥0.21` / `12.4K / 3.1K`。现在
    `UsageRow` 带 `sid`（provider 调用时从 `req.session_id` 写入），新增
    `usage.session`，侧栏显示真实的累计 tokens/成本；上下文水位由 UI 按模型族
    窗口估算（未知模型显示 "—" 而不是编一个百分比）。
  - 252 用例 / 2509 断言（新增 10 个 git 用例、5 个 UI 用例）。详见 current.md <53>。

<54>(WIP): 界面细节四修 + 历史分页 + 接线盘点
  - **history 宽度**：`max-width: 50%` + 窄容器时切到 `calc(100% - 20px)`。
    注意**不能**写成 `min-width: calc(100% - 20px)`——CSS 的取用宽度是
    `max(min-width, min(max-width, width))`，min-width 永远赢，会变成任何宽度都满宽。
    这个切换本质是不连续的，所以用 `.session-center` 上的 container query
    （它才是真正会变窄的盒子；媒体查询问的是视口，侧栏一开就答错）。
    实测：宽 914px 时列宽 840px（≈100%-20），窄 334px 时列宽 260px（同样 ≈100%-20），
    两者与输入框左右边完全对齐（composer 与 history 的百分比基准差 44px 的 padding）。
  - **规划面板回到悬浮窗**：`.plan-float`（absolute，左上），但仍是 `.history` 的
    **兄弟节点**而不是子节点——放回 history 内部会重建整个聊天子树并丢滚动位置。
    点开它不再改变列宽（列是"第二个侧栏"的观感，已撤掉）。
  - **侧栏标签动画**：body 移入 `.side-content`（absolute inset 0），切换时旧
    body 播 `anim-exit-left` 留在原地、新 body 播 `anim-slide-left` 且延迟 .18s，
    形成真正的双 view 错峰。两个坑：
    * 退出节点不消失：`_fs_ui_timeout` 没在 `-sEXPORTED_FUNCTIONS` 里，`schedule`
      的回调永远不会触发——加了导出，气泡 2 个 → 1 个。
    * 错峰延迟中途被清掉：第二次渲染（面板数据到达）时"上一个 key"已等于当前 key，
      新树不再带 inline delay，`patch_attrs` 就把活动节点的 delay 清成 0s，动画
      从头重播 → 看起来"闪一下就到位"。改为在整个进入窗口内**每帧重写** delay
      （`side_entering_key_` + 480ms 后清除标志）。
  - **历史分页**：
    * 后端新增 `session.turns {sid, before?, limit?}` → `{turns, start, total, hasMore}`，
      以及 `session.info`（除 turns 外的 meta/settings/systemOps），打开会话不再
      传输整份历史只为读模型名。
    * 首屏只取最后 30 轮；滚到顶部（24px 容差）取上一页并**前插**；`hasMore=false`
      后不再请求。
    * 前插保持阅读位置：记录"距底部距离"（`scrollHeight - scrollTop`），插完按新的
      scrollHeight 反算 scrollTop——按 scrollTop 锚定会正好跳动插入内容的高度。
    * **前插不重建**（关键）：消息身份从"向量下标"改成**会话内绝对轮次号** `uid`。
      下标身份会让每次前插把所有 key 整体挪位，整段历史重建，正好违背"固化历史不重建"。
      测试断言前插后旧消息仍是同一批 VNode 对象。
  - **贴底跟随**：`hist_follow` 由滚动边界事件驱动——到底部开、向上滚立刻关；
    流式 delta / tool.update / turn.done 之后（render 之后才有新 scrollHeight）
    若仍在贴底则滚到底。读历史时新内容绝不把视图拽下去。
  - **命令菜单后端化**：新增 `command.list` / `command.run`。
    每条命令 = `name + title + description + action`，action 是 `ui:<x>`（UI 自己处理：
    打开规划/新建会话/打开某个面板）或一个后端方法名（`session.suspend` 等）；
    skill（kind=skill）自动并入同一列表。UI 不再硬编码任何命令，之前只列 skill
    而且把内置 MCP（browser-control / lsp）也列进去了——它们没有 /name 调用形式。
  - **过程高度**：思考与工作过程都限 `min(10 行, 半页)` 并可滚动
    （`.think-body` / `.work-body`），展开过程不会把答案顶出视野。
  - **上下文水位改为精确值**：不再按模型名估算窗口。
    * `/models` 响应里读 `context_length` / `context_window` / `max_context_length` /
      `max_model_len`（含 `top_provider` 嵌套）写入 `caps.context_len`；
    * `usage.query` 返回 `lastIn`（最后一次请求的 prompt tokens）、`contextWindow`、
      `lastModel/lastProvider`（UsageSummary 新增 last 行）；
    * 侧栏 = `lastIn / contextWindow`；任一缺失就显示 "—"，不编造分母。
  - 255 用例 / 2580 断言。
  - **接线盘点**（120 个方法，UI 已用 72 个）：未接的 48 个里，多数是"设计上不该由
    UI 直接调用"的（lock.*、tool.invoke、steering.*、workflow.* 由 agent 侧驱动，
    subagent.spawn 由模型调用）。真正属于"界面应该有、还没做"的见下一轮清单。

<55>(TODO): 下一轮接线清单（按优先级）
  A. 会话生命周期（界面已有入口但没接全）
     1. session.fork（从某一轮分叉）—— 消息 hover 菜单
     2. session.rollback（回滚到某一轮）—— 同上，需确认框
     3. session.continue（中断后带引导续跑）—— loop.interrupted 提示条上的按钮
     4. session.cancelWake + session.suspend/resume 的状态回显（挂起按钮当前只发 suspend）
     5. session.setEnterSend（设置项"Enter 发送"目前只写设置，没下发会话）
  B. 会话列表与历史
     6. session.forget（忘记某会话的哪一级：菜单/记忆）
     7. session.refreshConfig（模型/权限变更后刷新 provider 运行时）
  C. Git 面板续做（后端已就位）
     8. git.log（提交历史列表）
     9. git.branches（分支列表 + 切换提示，切换本身需要新后端方法）
     10. git.showHead（diff 的"旧版本"对照视图）
     11. git.stage / git.unstage 的单文件入口已在用，补"按 hunk 暂存"需要新后端
  D. 用量
     12. usage.exportCsv（用量页的"导出 CSV"按钮）
     13. model.allEnabled 作为模型选择器的数据源（目前用 provider.listModels 聚合）
  E. Skill / MCP 管理
     14. skill.setEnabled / skill.remove / skill.create（设置-Skill 页目前只能导入和加载）
     15. mcp.import（设置-MCP 页缺"从目录导入"）
  F. 设置
     16. settings.unset（"恢复默认"按钮）
     17. settings.setIgnoreGlobal（项目级忽略全局设置）
     18. thinking.setLang（思考语言选择器的写入口）
  G. 其它
     19. workflow.*（工作流页整页未做；后端 define/list/start/get/retryStep/skipStep/steer 全在）
     20. plan.update / plan.review（规划面板只读，不能编辑步骤或评审）
     21. terminal.show / terminal.setEncoding（终端详情与编码切换）

<56>(SOLVED): 接线完成（21 项清单全部接上，UI 覆盖率 122 中 102）
  - **前置缺陷**：`append_turn` 从不写 `turnId`，而 fork/rollback/forget 都按
    turnId 找轮次——三个接口存在但永远返回 "turn not found"。现在落库时补
    `t_<hex>`，并且测试断言 id 唯一、可被 rollback 定位。
  - **A 会话生命周期**：
    * 消息 hover 出「分叉 / 回滚到这里 / 忘记」。分叉后直接打开新会话（分叉完
      还要去项目列表里找它，等于没分叉）；回滚走确认框（会撤销文件改动），
      确认框的按钮文字按动作变化（删除/回滚/撤销/忘记），并且带第二个参数
      `confirm_arg`（turnId）——通用"删除"标签配通用动作正是确认框要防的事。
    * `loop.interrupted` 不再是转瞬即逝的 toast：composer 顶部常驻一条
      「任务已中断 / 继续 / 忽略」，继续走 `session.continue`。
    * 挂起按钮改为等后端确认后再变标签，并 `session.loopState` 回读真实状态；
      `AgentHost::loop_state` 把"已请求挂起"优先报成 `suspended`（否则按钮会
      在用户按下后跳回"挂起"）。挂起时额外出现「取消唤醒」。
    * Enter 行为重新接回：设置项写 `session.default.enterSend` 并下发
      `session.setEnterSend`；textarea 带 `data-enter-send`，DOM 适配器的 keydown
      按它决定 Enter 是发送还是换行（以前这个设置被删掉就是因为"没效果"，
      而没效果的原因是 key 处理器根本不读它）。
    * 切换模型后补 `session.refreshConfig` + 重算上下文水位。
  - **C Git**：面板加了「改动 / 历史 / 分支」三视图（一个 tab，三个问题）。
    历史来自 `git.log`，分支来自 `git.branches`（切换分支会动文件，故意只展示
    并提示去终端执行）；diff 头加「对照 HEAD」用 `git.showHead` 看改动前的整份
    内容。
  - **D 用量**：导出 CSV 按钮（`usage.exportCsv`）。空路径以前会写到进程启动
    目录——没人找得到；现在后端默认写 `<data>/exports/usage-<ts>.csv` 并回传
    路径。模型选择器改用 `model.allEnabled`（跨 provider 一次取全，行上带
    providerId，切换供应商零请求）。
  - **E Skill/MCP**：Skill 卡片有启用开关和删除（仅用户导入的），新增「新建」
    表单（名称/标题/正文 → `skill.create`）；MCP 页「手动导入」改走
    `mcp.import`（整目录导入），不再假装走单条添加。
  - **F 设置**：文件页加「恢复默认」（`settings.unset` 清黑白名单与自动 git
    init）和项目级「忽略全局设置」（`settings.setIgnoreGlobal`，状态从
    settings.dump 的 `ignoreGlobal` 读回，不是猜的）；思考语言写入时同步
    `thinking.setLang`，后端缓存的 skill 描述语言才会立刻变。
  - **G 其它**：
    * 新增「工作流」侧栏面板：运行中的 run + 每步状态 + 失败步的「重试/跳过」
      与运行步的「引导」，以及可启动的定义列表。为此补了 `workflow.runs`
      （按 sid 列 run；之前只有 workflow.get(runId)，UI 无从发现 run）。
    * 规划面板可编辑：逐步内联编辑、删除、添加步骤，均整体写回 `plan.update`；
      底部有评审（通过/驳回/按意见修改 → `plan.review`）。
    * 终端标签加「编码」按钮，循环 系统默认 → UTF-8 → GBK，走
      `terminal.show` + `terminal.setEncoding`。
  - **仍未接的 20 个**（都是"不该由 UI 直接调用"）：`lock.*`（权限门的内部锁）、
    `tool.invoke` / `subagent.spawn` / `steering.*`（模型侧调用）、
    `workflow.define`（定义由内置表提供，暂无编辑器）、`session.get`（已被
    info+turns 取代）、`session.open/close/append`（内部生命周期）、
    `project.get` / `settings.get`（dump 已覆盖）、`browser.load`（skill 加载走
    command.run）。这些保留为接口，不算缺口。
  - 268 用例 / 2667 断言（新增 7 个 wiring 用例 + 6 个 UI 用例）。

