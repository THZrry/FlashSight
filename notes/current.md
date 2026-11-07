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

- P0+P1+P2+P3 完成。roadmap 30-agent.md 已勾选（剩 search_web 调研）。
- 下一步：P4 UI（等 WhaleUI；fs_ui 空壳+事件类型先行）或收尾提交。

<11>(SOLVED): P0-P3 回顾审计出的缺口（用户 2026-10-11 提出复查，已全部落地）：
  1. 设置面后端键缺失：模型偏好（model.pref.*）、子代理开关/模型持久化
     （subagents.<name>.enabled/model）、长期记忆（存储+读写 API）、外观主题/字体键、
     浏览器/电脑控制开关（browser.*/computer.*）、快捷键表（shortcuts.*）。
  2. 环境变量合并注入逻辑缺（terminal.md 5：全局+项目合并，spawn 时注入）——只有键，无函数。
  3. Anthropic 会话调不通工具：HistoryBuilder 只产 OpenAI 格式 tool_calls，
     Anthropic 的 tool_use/content-blocks 装配缺失。
  4. x-opencode-session 套餐协议未落地（等调研子代理结果）。
  5. 单会话持久记忆缺：需新增 summarize_session 工具（每轮记要点到 turn 要点字段），
     HistoryBuilder 压缩时把要点附加到上下文（平衡调用与记忆，支持几百轮）。

<12>(SOLVED): 缺口补齐落地细节（39 用例 340 断言全过）：
  1. 设置键：model.pref.*/subagents.*/model.longMemory/ui.theme-font/ui.* 全部键+默认值；
     merged_env()/merged_cli_tools()（全局+项目覆盖，ignoreGlobal 只取项目）。
  2. Anthropic 装配：HistoryBuilder.build_anthropic（system 单独字段 + text/tool_use/tool_result
     blocks；兼容扁平与 OpenAI 原生 toolCalls；tool 结果经 turn.toolResults 回填）。
  3. 持久记忆：summarize_session 工具（points 数组 -> session.keypoint 事件）；
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
    A2 check_dir；A3 file_regex（QuickJS 引擎移植，见 <15>）；A4 权限门（full 才可项目外）；
    A5 噪声检测（罕见 script 短串+引号佐证）；A6 list_dir 基础/详细双模式。
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
  - fetch_url 回调也接上（NetClient，method/headers/body/timeoutMs/sizeCap/verifyCert）。
  - screenshot/readImage 改为明确的 not-supported（之前经 net_fetch 转发是错的：
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
  - MCP/Skill 做实：McpRegistry upsert/set_enabled/remove/dump/load；FsApi mcp_list/
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
    （settings.detectShells 结果）+ 可选名称，terminal_spawn（interactive=true）
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
    `list_dir` 走 `resolve_by_perm`。`AgentHost::loop_for` 现在从 full.json 的
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
    model / summary / open；`subagent_list` 本来就带 `full`，现在解析并渲染成
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
    subagent_spawn 由模型调用）。真正属于"界面应该有、还没做"的见下一轮清单。

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
     15. mcp_import（设置-MCP 页缺"从目录导入"）
  F. 设置
     16. settings.unset（"恢复默认"按钮）
     17. settings.setIgnoreGlobal（项目级忽略全局设置）
     18. thinking.setLang（思考语言选择器的写入口）
  G. 其它
     19. workflow.*（工作流页整页未做；后端 define/list/start/get/retryStep/skipStep/steer 全在）
     20. plan_update / plan.review（规划面板只读，不能编辑步骤或评审）
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
    `mcp_import`（整目录导入），不再假装走单条添加。
  - **F 设置**：文件页加「恢复默认」（`settings.unset` 清黑白名单与自动 git
    init）和项目级「忽略全局设置」（`settings.setIgnoreGlobal`，状态从
    settings.dump 的 `ignoreGlobal` 读回，不是猜的）；思考语言写入时同步
    `thinking.setLang`，后端缓存的 skill 描述语言才会立刻变。
  - **G 其它**：
    * 新增「工作流」侧栏面板：运行中的 run + 每步状态 + 失败步的「重试/跳过」
      与运行步的「引导」，以及可启动的定义列表。为此补了 `workflow.runs`
      （按 sid 列 run；之前只有 workflow.get(runId)，UI 无从发现 run）。
    * 规划面板可编辑：逐步内联编辑、删除、添加步骤，均整体写回 `plan_update`；
      底部有评审（通过/驳回/按意见修改 → `plan.review`）。
    * 终端标签加「编码」按钮，循环 系统默认 → UTF-8 → GBK，走
      `terminal.show` + `terminal.setEncoding`。
  - **仍未接的 20 个**（都是"不该由 UI 直接调用"）：`lock.*`（权限门的内部锁）、
    `tool.invoke` / `subagent_spawn` / `steering.*`（模型侧调用）、
    `workflow.define`（定义由内置表提供，暂无编辑器）、`session.get`（已被
    info+turns 取代）、`session.open/close/append`（内部生命周期）、
    `project.get` / `settings.get`（dump 已覆盖）、`browser.load`（skill 加载走
    command.run）。这些保留为接口，不算缺口。
  - 268 用例 / 2667 断言（新增 7 个 wiring 用例 + 6 个 UI 用例）。


<57>(SOLVED): 打磨批：翻页后折叠失效（真根因）+ 中断原因 + 紧凑靠右 + 紫点 +
  文件预览行号 + 主页动画回退
  - **严重 bug：向上翻页后 summary 点不开**。根因不是 id 冲突，而是消息 handler
    捕获的是**数组下标**：缓存的 VNode 保留创建时的闭包，前插 30 条后下标整体
    位移，点第 5 条实际切换的是第 35 条——看起来就是"点了没反应"。修复：handler
    一律用稳定 `uid` 解析（`find_message_by_uid`），渲染循环和解析用同一套
    uid 回退规则（live 消息 = 1e9 + 位置）。复制、折叠、工具块、分叉/回滚/忘记
    全部改走这条路径。
  - **"输入什么就报任务已中断"**：不是误报，是模型调用真的失败（demo 里的
    DeepSeek key 是占位符）。但 UI 只显示"任务已中断"，看起来像界面 bug。
    现在中断条显示后端给的原因（`loop.interrupted.error`，含 HTTP 状态）。
  - **主页四页动画统一**：用户明确要"项目"原本那种纯淡入。四个子页全部恢复
    `anim::kFade`（纯透明度 0.05s，无位移、无退场覆盖层），并删掉用量页自己
    多出来的 `anim-enter`（它覆盖了外层动画，导致那一页单独不一样）。
    中途试过"左上淡入 + 双视图退场"，被否掉，已完整回退。
  - **紧凑时右侧按钮靠右**：600px 以下 `.grow` 被隐藏，左右两组塌在一起。
    改为 `.ct-left { flex:1 }` + `.ct-right { margin-left:auto }`，不再依赖
    那个会被隐藏的 spacer。实测紧凑模式 gapRight=0。
  - **紫点 = 后台在跑**：`session.list` 增加 `terminalCount`（该会话仍运行的
    终端数），`session_dot_with` 在"没有 turn 在跑但有活终端"时输出
    `.background`（紫色 #a06cd5），title 写"后台运行中（终端 N 个）"。
    优先级：等待确认 > 运行中 > 上次出错 > 后台 > 已完成 > 空闲。
  - **文件预览**：加行号槽（与代码同一个滚动容器，行号不会和行错位）+
    「复制相对路径」按钮（模型和终端认的是相对路径）。
  - **`IDom::schedule` 的真 bug**：`sched_cbs()[++sched_seq()] = cb` 之后又
    `fs_dom_timeout(ms, sched_seq())`，自增了两次——回调存在 N、定时器用 N+1，
    永远找不到，退场节点**永远不删**。改为取一次 id。这是"侧栏切换后 DOM 里
    一直有 2 个 body"的根因。
  - 268 用例 / 2672 断言。浏览器实测：翻页后点第 5 条的折叠只开第 5 条；
    紧凑 gapRight=0；紫点正确；文件预览行号 1/2 + 复制按钮；四页均 anim-fade。


<58>(SOLVED): 探索/搜索的卡片动画补回（和"项目"同源）
  - 用户喜欢的是"项目"里**每张卡片各自上浮 6px + 淡入**（`.proj-card` 上的
    `anim-enter`），不是整页动画。上一轮把四个子页统一成纯淡入时，只保留了
    **页面级**的 `anim-fade`，而 探索/搜索 的条目从来没进过那条卡片规则，
    所以看起来"又没动画了"。
  - 修复：`.explore-sec`（区块卡）、`.explore-row`（能力行）、`.search-row`
    （结果行）、`.explore-empty` 加入同一条 `anim-enter` 规则 —— 与 `.proj-card`
    完全同一套动效，不再各写一份。
  - 错开：项目卡片 40ms/张；探索按**区块**错开（0 / 60ms，不按行——能力列表
    可能很长）；搜索 30ms/条，上限 240ms（宽查询不会让最后一条等一秒）。
    延迟走 inline style + `animation-fill-mode: backwards`，避免元素先出现再跳。
  - 页面级仍是纯 `anim-fade`（0.05s，无位移），两套动画分工明确：页面淡入、
    内容卡片上浮。
  - 实测：探索两个区块 delay 0s/0.06s、条目 transform translateY(6px)；
    搜索三条 0/0.03/0.06s。268 用例 2672 断言。

<59>(SOLVED): Web / Desktop 双版本 + WebViewDom 适配器（webview 作为 WhaleUI 的后备）。
  - 三变体共用同一套 UI（ui_app/vdom/components 未改），只换 IDom：
    Web(默认, WasmDom) / Desktop-webview(WebViewDom) / Desktop-WhaleUI(WhaleDom)。
    各自独立构建目录（build_web / build_desktop / build_whale），build.bat 一键切换。
    --webview=y 与 --whaleui=y 互斥（都会替换 ui_server.cpp 并各自提供入口）。
  - 新增 src/ui/ui_dom_webview.cpp（WebViewDom）+ src/ui/ui_main_webview.cpp
    （原生窗口 + 内嵌资源 + NativeBackend）。webview 0.12.0 + WebView2 SDK 放
    3rdparty/webview、3rdparty/mswebview2（脚本已补拉取，见 doc-internal/thirdparty.md）。
  - 三变体都内嵌 web/（xmake 的 pack_web_assets 生成 ui_assets_gen.hpp）。Desktop
    把 CSS 内联进 `<style>`、woff2 转 data: URI —— webview_set_html 用
    NavigateToString，文档没有 base URL，相对路径无法解析。
  - 适配器关键设计：webview_eval 无返回值、绑定是异步 Promise，所以
    (a) 节点 id 由 C++ 分配（JS 按 id 存表，无需回传）；
    (b) get_value/is_focused/scroll_* 走 C++ 镜像，用户改动经 fs_ui_push 回传
    （在事件处理器之前送达，所以处理器里读到的是新值）。
  - 坑：
    * webview_init() 必须在第一次 set_html 之前调用，否则 glue 不进当前文档 → 全黑。
    * webview 绑定是**位置参数**的 JS 函数，C 侧收到 JSON 数组 [a,b,...]，
      不能把数组当单个参数传。
    * insert_before 忘了把 ref 传给 JS → diff 重排全部追加到末尾，点击后整页变空。
    * 链接需要 shlwapi（内置 WebView2 loader 用 PathFindFileNameW）。
    * xmake 只在 target hook 的**函数体内**暴露 io/os/path；打包逻辑若抽成脚本级
      函数会报 nil（"attempt to call a nil value" / "index a nil value"），必须内联。
  - 实测：Web 版编译通过（139 个内嵌资源）；Desktop 版编译通过，窗口渲染正常，
    设置页双栏/表单/开关、主页子标签切换均可用。
  - 文档：doc-internal/ui-webview.md（三变体、适配器设计、踩坑、已知限制）。

<60>(SOLVED): 构建目录归拢到 build/ + 三变体重命名 + GitHub Actions 多平台打包。
  - 变体命名统一为 Web / Webview / WhaleUI（xmake 的 --webview / --whaleui），
    输出目录不再是 build_web/build_desktop/build_whale 三个兄弟目录，而是
    build/web、build/webview、build/whaleui（build.bat 用 -o 指定）。
    .gitignore 补上 build/ 与所有日志/临时文件（*.log、build*.txt、run*.txt、
    probe_*.txt）；仓库根目录清掉 24 个历史日志。
  - 三个变体**都**编译内嵌 HTTP 服务（此前只有 Web 版有）。远程接入入口仍是
    TODO（main.cpp 注释已写明将来 --no-window 只跑服务），但 server 已经先起来，
    桌面版启动时也会打印 "UI server: http://127.0.0.1:PORT/"。
  - 跨平台化的坑（都是 CI 才暴露的）：
    * emcc 检测按 ';' 切 PATH → Linux/macOS 上永远 false，wasm 被静默跳过，
      构建出的 Web 包没有 fs_ui.js。改成按 host 选 ';' / ':'。
    * wasm target 只 set_plat 不 set_arch，--arrm64 会漏进 wasm 平台
      （wasm 只有 wasm32/wasm64）→ arm64 的 CI job 直接配置失败。补 set_arch("wasm32")。
    * mbedtls 此前硬编码 3rdparty/mbedtls-3.6.4/build/*.a（本地 cmake 产物），
      CI 没有。改为：有这个树就按路径链；没有就 add_requires("mbedtls 3.6.7")，
      且此时**不能**把 3.6.4 的头文件放进 include path（否则头是 3.6.4、库是 3.6.7）。
    * -std=c++14 会定义 __STRICT_ANSI__，glibc 下 popen/mkstemp/nanosleep 不声明。
      非 Windows 加 _DEFAULT_SOURCE / _DARWIN_C_SOURCE；并补 -pthread、-ldl。
    * webview 的 Linux 后端要 gtk+-3.0 + webkit2gtk-4.1，用 find_package("pkgconfig::…")
      取结构化 links/linkdirs，别手工 split pkg-config 输出；macOS 只需
      add_frameworks("WebKit") + dl（webview.cc 是纯 C++ 走 objc_msgSend，不用 .mm）。
    * Windows 的 compat-mingw / mswebview2 头文件目录只在 is_plat("windows") 时加。
    * set_policy("build.across_targets_in_parallel", false) 在 xmake 3.x 已废弃，
      改 build.fence（否则每次构建都刷警告）。
  - 依赖脚本统一成一份 Python（scripts/fetch_3rdparty.py），.sh/.bat 只是壳：
    curl+unzip+tar 在干净的 Windows runner 上不齐，urllib+zipfile+tarfile 到处都有。
    幂等（已解压就跳过），支持 PROXY / SKIP_WEBVIEW / WHALEUI_DIR。
  - 打包脚本 scripts/package.py：每个平台每个变体一个包
    flashsight-<version>-<platform>-<variant>.zip|tar.gz。Windows 上按 PE 导入表
    决定要带哪些运行时 DLL（不写死 MinGW 列表，llvm-mingw 的 libc++.dll 也能带上）；
    Windows 出 zip，其他出 tar.gz（保留可执行位）。
  - .github/workflows/build.yml：tag（v*）与 release published 触发，矩阵
    windows-x86_64(MSYS2 mingw64) / windows-arm64(llvm-mingw 交叉) / macos-arm64 /
    linux-x86_64 / linux-aarch64，每个 job 编 Web + Webview 两版并出两个包，
    最后 softprops 挂到 release（tag 推送时建 draft）。WhaleUI 不在 CI 里——
    它需要 WhaleUI 仓库与其引擎构建产物，本地用 build.bat whaleui。
  - 实测：本机 web/webview 两版重新编译通过；打出的两个 zip 解压后直接运行，
    Web 版 200 提供 index/wasm（1,592,654 字节）与 /api/call，Webview 版窗口正常。

<61>(SOLVED): 网络"连不通"的三个真 bug（不是网络问题）+ 多轮对话 + 定时任务。
  - 起因：界面里发消息报 "network io error"，但 curl 与同一进程里的临时请求都正常。
  - 根因 1（最致命）：httplib 的 TLS 宏只定义在 fs_core 上。ui_server.cpp 属于
    fs_ui，也 include 了 httplib.h；没带 CPPHTTPLIB_MBEDTLS_SUPPORT 编出来的那份
    头文件里 SSLClient 的实现退化成普通连接，链接器选中了它，于是所有 HTTPS 请求
    都报 "Could not establish connection"，而单独编译一段同样的请求却能成功。
    修法：把 CPPHTTPLIB_MBEDTLS_SUPPORT 提到项目级（xmake 顶层 add_defines）。
  - 根因 2：mbedtls 链了 MSVC 编的 .a。3rdparty/mbedtls-3.6.4/build 里的归档成员
    是 *.obj（MSVC），链进 MinGW 程序后两套 CRT 混用，请求完成、ClientImpl 析构
    时在 RtlFreeHeap 里 SIGSEGV（gdb 抓到）。修法：mbedtls 一律用 mbedtls_src
    target 从源码编（同一工具链），不再用预编译树。
  - 根因 3：provider 路由只取第一个启用的连接。配了 Anthropic + DeepSeek 时，
    "deepseek-chat" 被发到 api.anthropic.com，回 403。修法：先按"哪个 provider
    的模型表里有这个 model"匹配，认不出再回退第一个启用的。
  - 多轮对话其实是假的：run_turn 只发 [system, 当前 user]，HistoryBuilder 根本没
    被调用；而且 persist_turn 从未被调用，会话文件里从来没有 turn。两者都接上
    （AgentLoop::load_history / persist_turn，Host 注入），现在"我叫 Ada"下一轮能记住。
  - 中断后无法再发消息：AgentLoop::send 用 state_ != kIdle 判断忙，而失败后状态
    停在 kTurnDone（其实是空闲），于是后续每条消息都进引导队列永不发送。
    改为 kIdle 与 kTurnDone 都算空闲。
  - 换模型不生效：loop 只在创建时读一次模型。新增 set_model，并把会话模型从
    sessionSettings 传进 AgentHost::send。
  - 定时任务：WakeScheduler 早就写好，但 (a) 没有任何接口能创建定时唤醒，
    (b) on_timer_tick 从未被调用。补 session.schedule / session.pendingWakes，
    并让终端轮询线程每 60ms 调一次 on_timer_tick。UI 在输入框上方（与中断条
    同一位置）显示倒计时与取消，自动模式下工具条多一个"定时"按钮。
  - 输入框 placeholder 跟着 Enter 设置走（Ctrl+Enter 模式下不再说"Enter 发送"）。
  - 调试开关：FS_NET_DEBUG=1 打印每条请求的 host/path/代理/TLS/结果；
    map_httplib_error 现在带上 httplib 的原始错误文本，不再一律 "network io error"。
  - 验证：web 变体重编通过；268 用例 2672 断言全过；打包后的 exe 直接运行能提供
    index/wasm/API；对话连续两轮且第二轮记得第一轮内容；定时唤醒 2 秒后触发
    wake.fired。

<62>(SOLVED): 重新打开会话看不到自己的输入 + 全局右键菜单。
  - 用户历史不显示：后端返回正常（8 轮都在），是 UI 的 turn_to_message 只读
    `text` / `streamSummary`。user turn 把输入存在 `promptRaw` 里，于是每条用户
    消息都渲染成空白——答案都在、问题全没了。改为依次回退
    text → promptRaw → streamSummary。
  - 右键：浏览器原生菜单是**文档级默认行为**，只在节点上绑 contextmenu 拦不住，
    没绑定的地方照样弹「后退/重新加载/检查」。两个 DOM 适配器启动时装一个
    document 级监听：INPUT/TEXTAREA/contentEditable 放行（剪切复制粘贴是浏览器
    能力，应用做不了），其余 preventDefault。
  - 应用菜单：AppState::ctx_* + overlay 层 fixed 定位，坐标来自指针。适配器把
    contextmenu 的事件值从"元素文本"改成 "x\ty" 视口坐标（三个后端都改了），
    否则菜单没有位置可开。
  - 菜单项按对象给，只留能用的：消息=复制/分叉/回滚/忘记；标签页=重命名/关闭；
    会话行=打开/重命名/删除；输入区=清空/复制输入内容。Esc 与点空白处关闭。
  - 顺手把 placeholder 跟着 Enter 设置走（上一批遗留）：Ctrl+Enter 模式下显示
    "Ctrl+Enter 发送，Enter 换行"，不再硬编码"Enter 发送"。
  - 浏览器实测：会话 s_dec6176ec0d4 的 4 条用户消息全部显示；右键消息出
    "复制/从这里分叉/回滚到这里/忘记这一轮"；右键会话行出"打开/重命名/删除会话"；
    空白处 contextmenu 被 preventDefault，textarea 未被拦截（保留原生菜单）。

<63>(SOLVED): 真流式 + 计时 + 右键补齐 + 上下文水位 + 模型属性自动填充。
  - 流式原来是假的：fs_api 写死 `stream = false`，所以内容整段刷出。现在是真的
    SSE——NetClient 加 request_stream（httplib 的 ContentReceiver，按 read 边界
    回调），ProviderClient 用 SseReader 拼 `data:` 行，每段文本立刻 on_delta →
    AgentLoop 发 stream.delta。非流式仍支持：服务端忽略 stream 只回整段 JSON 时
    saw_sse=false，走正常解析再补发（否则回复直接消失，5 个用例抓到）。
  - UI 端两个真 bug：① delta 判断只 `role != assistant`，上一轮的答案也是
    assistant，于是新一轮文本被追加到旧消息上（两个答案粘一起）；改判
    `!streaming`。② 每条 delta 都 render 会把浏览器事件循环饿死（下一次 poll 发不
    出去，看着像"卡住"）；改为标脏 + pump 每 50ms 重绘一次。另外 poll 加在途保护、
    drain_events 每批上限 128、事件环 512→8192（流式一轮就有几百条）。
  - 计时：后端在 turn.done 里带 startedAt/endedAt（它知道请求何时发出），流式期间
    每秒刷新已用时长；结束后在 hover 那排按钮旁边常显"时长 · 结束时刻"。
  - 右键：左栏会话行没绑（只有项目卡片有）；输入框之前放行原生菜单，现在统一由应用
    菜单接管（剪切/复制/粘贴/全选，粘贴走异步剪贴板 API）。菜单外的**任何**按下都
    关菜单且不吞事件（bind_outside_click），所以右键另一处会直接换成那处的菜单——
    之前用全屏遮罩，第二次右键被吃掉，表现为"什么都没有"。
  - 上下文水位：① 流式响应默认不带 usage，加上 stream_options.include_usage，
    否则每次流式调用的 prompt tokens 都是 0（水位恒 -1）；② 用户只改过 enabled 的
    模型覆盖了 API 的 context_len，resolve() 改为合并而非覆盖；③ 取回的模型列表落盘
    （provider.apiModels），重启后 ctx 不再丢；④ 百分比最低显示 1%，旁边常显数字
    （1M 窗口下几千 token 整除成 0，看起来像没算）。
  - 模型属性自动填充：一次"获取模型列表"就补齐 name / context_len / max_output /
    多模态（input_modalities）/ 思考档位（effort.supported_levels）/ 工具与结构化
    输出能力，用户不必手填。
  - 浏览器实测：流式期间采样到字符数递增（337→959→1587→2299）、实时计时
    1.0s→2.0s→4.0s、贴底跟随正常；右键消息/标签页/左栏会话行/输入框各自的菜单
    都对，且第二次右键会切换菜单；输入框菜单出"剪切/复制/粘贴/全选/清空输入"；
    水位显示 1%。268 用例 2701 断言全过。

<64>(SOLVED): 流式"像是一次性生成完再喂"——根因是浏览器定时器节流。
  - 上一轮虽然做了真 SSE 后端（ProviderClient 解 SSE、AgentLoop 边收边发），但前端
    还是**轮询**取事件。实测：页面里 `setInterval(120ms)` 的真实间隔是 1031ms——
    浏览器把被遮挡/后台页面的定时器压到 ~1Hz。而模型每秒产生约 150 条 delta，
    每次 poll 最多取 128 条，于是前端永远追不上，等生成结束才开始补完。
    这也解释了为什么"后端事件时间戳看着是流式的、界面却像一次性刷出"。
  - 修法：服务端加 `GET /api/stream`（SSE，chunked + condition_variable 唤醒），
    前端改用 `EventSource`；`poll()` 只确保连接打开，不再拉取。
  - 重绘也改成数据驱动：delta 到达时直接 render（SSE 帧是普通任务，不受定时器
    节流），25ms 下限防连发；不再依赖 setInterval 刷帧。计时同样在 delta 里盖章。
  - 验证（浏览器 MutationObserver 采样）：一次 250 词回复产生 47 次重绘，字符数
    4→46→93→142→206→…→1125 平滑增长，实时计时 0.0s→1.6s；结束后常显
    "2.7s · 09:19"，操作按钮仍 hover 才出现。
  - 教训：只要 UI 的刷新节奏由定时器驱动，就必须先确认这个定时器没被浏览器节流。

<65>(SOLVED): 模型可用性校验 + Web 服务设置（远程+密码）+ 密码遮罩/鉴权。
  - 会话记住的模型可能失效（被禁用/供应商被删）：打开会话时核对 model.allEnabled，
    不在列表就清空模型框、按钮标警告、**禁用发送**并提示；send_message 里有同一道
    闸。无后端时跳过（UI 测试/纯预览的演示路径）。真发出去只会失败在 provider 层，
    错误信息里根本不含"模型不存在"。
  - 设置-权限-Web 服务：监听地址（仅本机 / 局域网远程）、端口、访问密码，改动实时
    生效。httplib 不能在 listen 后换绑定地址，只能 stop+start。
  - **重启不能在被服务的那条请求线程上做**：stop() join 监听线程，监听线程退出又
    join 线程池，而当前请求就在池里 → 死锁（浏览器看到请求挂住）。改成 detached
    线程 + 延迟 150ms 让响应先写回。stop() 还会摘掉事件监听器，重启前要重新注册。
  - 鉴权只约束远程：回环始终免密（桌面版自己就通过这个服务器说话）。/api/auth 探测、
    /api/login 换 token、之后 /api/* 带 Bearer。**页面外壳必须公开**——它不含数据，
    且必须先加载出来才能显示密码框；一开始把 `/` 也拦了，远程拿到的是 401 JSON。
  - 密码遮罩是 index.html 里的纯 HTML/JS（wasm 挂载前就要能拦住）。模块可能在遮罩
    期间就加载完，那时 onRuntimeInitialized 会跳过启动 → 登录成功后必须由 startApp()
    补启动，否则登录成功但界面空白。
  - 顺带修：`fs_ui` 没把 web_assets 声明为输入，改 index.html/style.css 不会触发
    重新打包，内嵌资源一直是旧的（这次改 index.html 完全没生效才发现）。
  - 实测：远程访问出密码框、错密码提示且保持遮罩、对密码关遮罩并挂载（3 个项目卡
    渲染出来）；无 token 的 /api/call 回 401、带 token 回 200；/ 与 style.css 公开；
    设置切 127.0.0.1 后局域网连不上、切回 0.0.0.0 又能连。269 用例 2707 断言全过。

<66>(SOLVED): 三种访问方式是并列的 + 端口修改真正生效。
  - 用户指出：本地服务只是和 webview、WhaleUI 并列的访问方式，有原生界面就不该再起
    服务。上一版让桌面版也起服务器，结果是"两个界面、两个端口"的困惑。
    改：只有 Web 版启动内嵌服务（那里服务就是 UI），Webview / WhaleUI 只开窗口。
  - 端口"没生效"的真因：`text_input` 的 on_input 每敲一个字符就 `set_setting`，
    于是"1"→"18"→"189"… 逐位重绑；每次重绑都关掉还在传下一个按键的那条连接，
    最后一次可能落在没人指定的端口上。改成**只在 Enter 提交**，输入过程中只更新
    本地 draft（否则重渲染会用保存值把正在输入的内容抹掉）。
  - 换端口后页面必然失联（Web 版的 UI 就是从这台服务器加载的，无法跟着换源）。
    重绑前先通过 SSE 推 `server.moved`，页面显示"服务已更换地址"卡片 + 新地址链接。
  - `restart()` 现在是异步的（detached 线程 + 400ms 让响应和 server.moved 先送达），
    所以状态查询要能识别"正在重绑"，否则设置页读到的是旧端口。加 `rebinding` 标志
    并延迟重读一次。
  - 实测：webview 版启动后端口 18997 完全不通（无服务）；Web 版改端口 56566→18777
    →18788 每次都真的重新监听，日志逐条打印；旧页面收到"服务已更换地址"并给出正确
    的新链接；新地址打开后 3 个项目卡正常、SSE readyState=1。269 用例全过。

<67>(SOLVED): 重绑不换端口 + 换端口时页面自动跳转。
  - 用户指出：重启（改密码/改地址）不该把端口换掉——刚释放的端口马上就能再绑，不该
    当作"不可用"。真因是重绑走了 `port = 0` 那条路（0 = 启动时自动选），于是每次重绑
    都重新随机。现在只有明确写了不同端口才换，否则沿用当前端口。
  - 重绑顺序改成**先绑新端口、再关旧监听**。原来先 stop 再 start，中间有一段没有任何
    监听的窗口，页面轮询正好落在那里就失败；先绑也顺带验证了新端口可用，绑不上就保留
    原监听并把错误显示在设置页。
  - 路由注册抽成 `configure(Server&)`，重绑对新实例重放（httplib 的 Server 不能改绑定
    地址，必须换实例）。
  - 页面自动跳转：`server.moved` 只带**端口**，URL 由页面用自己的 hostname 拼（之前发
    绝对地址，远程客户端会被送到它自己的 127.0.0.1；而且用的是配置里的 port，自动模式
    下是 0，于是跳到了 `:0`）。收到后先 `no-cors` 探测新端口，通了才 replace —— 立即
    跳会落在浏览器错误页。探测失败 6s 才出"服务已更换端口"卡片；SSE readyState=2 时
    也提示，避免页面无声死掉。
  - 设置页的 "Web 服务" 只在 Web 版显示（`backend_name() == "wasm"`）：桌面版没有服务。
  - 实测：改密码/改地址后端口保持不变（53999 → 53999）；改端口 53888→53999 后旧页面
    自动跳到新地址；绑到被占用端口时保留原监听。269 用例 2707 断言全过。

<68>(SOLVED): Web 服务改端口/改地址真的生效了（根因是 SSE 把 stop() 卡死）。
  - 用户报告："输入端口没有反应"、"改本地/局域网把本地改成了 ::65535"、
    "0.0.0.0 没有服务"。用 curl 直接打 /api/call 验证：改 host、改 port 在 HTTP
    层全都成功——所以问题不在设置写入，而在重绑本身。
  - **真正的根因**：页面永远挂着一条 `/api/stream`（SSE）。SSE 的 chunked 处理器
    不会自己结束，它一直在 `cv.wait_for` 等下一个事件；而 httplib 的 `stop()` 会
    join 监听线程，监听线程退出时 join 线程池，这个 SSE 处理器正占着池里一个线程。
    于是 `stop()` 永远阻塞 → 重绑线程卡死 → 旧监听一直没关、新监听也没起来 =
    "改端口没反应 / 0.0.0.0 没有服务"。HTTP 层单独测（不挂 SSE）时全都正常，就是
    这个原因。
  - 修法：加 `stopping` 标志。`stop()` 先置位并 `notify_all()`；SSE 处理器每次醒来
    先看这个标志，置位就 `return false` 结束这个请求（chunked provider 返回 false
    会让 httplib 以 Canceled 退出循环），线程池才能 join 成功。重绑里换上新实例后
    再清掉标志（重绑不走 `start()`，得手动清）。同时把 SSE 的 keep-alive 等待从
    15s 降到 500ms，让 stop() 最坏也就等半秒。
  - 端口框现在 **Enter 或失焦**都提交（`text_input` 新增 on_blur），提示文案改成
    "回车或点到别处生效"——之前只有 Enter 生效，点了别处就当没改过。
  - 端口输入改成**校验**而不是静默钳位：非数字或超出 0-65535 一律拒绝并 toast
    提示，不再 `p > 65535 → 65535`。配置里那个 65535 就是钳位留下的假端口。
  - 设置页显示了真实的局域网 IP（`UiServer::lan_ip()`：开一个未连接的 UDP socket
    connect 到 8.8.8.8、读 getsockname，跨平台且自动跳过 down/虚拟网卡），不再是
    没法照抄的 `<本机 IP>` 占位符。状态刷新改成 rebinding 时轮询到落定（最多 20
    次），不再只延迟重读一次。
  - 实测：页面挂着 SSE 时改 host/port 都在 10ms 内返回且监听真的搬到新地址
    （0.0.0.0:8850）；SSE 在重绑时被服务端正常关闭（EventSource 会自动重连）。
    270 用例 2710 断言全过；web / webview 两个变体都编译通过。

<69>(SOLVED): 改端口/切地址时闪退 + 页面失联 + 端口"自己变了"。
  - 用户报告：改端口后本地也变了，后面什么操作都做不了；从"Web 服务"切到本地、
    再改端口，程序直接闪退且没有报错。用户猜是 join 监听的处理有问题——判断对了。
  - **闪退根因**：`restart()` 每次调用都 `std::thread(...).detach()` 一个新线程。
    连续两次设置（切地址、再改端口）就有两个线程同时进 `stop()`；第二个线程对
    已经 join 过的 `std::thread` 再次 `join()` 会抛 `std::system_error`，而它在
    detached 线程里没人捕获 → `std::terminate`，进程无声消失。**改成单个常驻
    worker 线程**：`restart()` 只把最新的目标设置放进槽位并唤醒，合并突发的多次
    设置（表单场景就是"最后一次生效"）。worker 在 `~UiServer` 里置 quit 并 join。
  - **页面失联根因**：`server.moved` 在 `restart()` 里就进了事件环，但 `do_rebind`
    随后是「先 stop 旧监听、再起新监听」。中间那段时间旧页面已经连不上、新端口
    还没起来，页面探测失败就停在死页面上——用户看到的"什么操作都做不了"。
    改成**新监听先 accept，再退役旧监听**：换入新实例、起 listen、等 `is_running`，
    然后才给旧实例置 stop 标志并 join。端口变化时旧 socket 再多活 1.2s，给页面
    完成探测 + `location.replace` 的时间。
  - **停止标志必须按"代"区分**：原来 `Impl::stopping` 是一个共享的 atomic。新旧
    监听短暂并存时，旧监听退役会把标志置位，新监听的 SSE 也跟着被杀。改成每次
    `start()`/重绑都新建一个 `shared_ptr<atomic<bool>>`，SSE 处理器捕获自己那一代
    的 flag。
  - **listen 线程不能捕获 `this`**：`std::thread([this]{ impl_->svr->listen... })`
    在线程真正跑起来时读的是**当前**的 `impl_->svr`，重绑换过实例后就会去 listen
    新实例（而它已经有自己的 listen 线程）。改成按值捕获 `httplib::Server*`。
  - **`--port` 与配置不同步导致"端口自己变了"**：启动时 `--port 8930` 只用于本次
    `server.start()`，配置里仍写着旧端口；之后任意一次改地址都会
    `apply_server_settings()` 重新读配置，把端口悄悄改回旧值。现在 `--port` 会写回
    `server.port`（`apply_server_settings` 在服务未运行时直接返回，避免启动前误重绑）。
  - **`/api/conn-lost` 页面兜底**：连接彻底断开时不再只给一个"重新加载"，而是提供
    端口输入框（预填当前端口）+ "打开新端口" + "重试当前地址"。
  - 实测：`--port 8930` 启动后依次 切局域网 → 切本地 → 改端口 8931，进程存活、
    端口正确；10 次快速 host 切换全部 200 无崩溃；页面内改端口 8931→8932 后标签页
    自动跳到新地址、SSE readyState=1、界面正常渲染。270 用例 2710 断言全过，
    web / webview 两个变体都编译通过。

<70>(SOLVED): 变体构建全在 xmake 里 + webview 资源没嵌入 + tool call 没做。
  - 用户指出三件事：① 自己编译时 webview 资源没嵌入；② 选 WhaleUI 却打包成 Web；
    ③ 每次切变体都要全量重编（"把 main 提出来只需要重编一个文件才对"）；④ tool
    call 完全没做。并且明确否掉了外部脚本路线："你不要做 build.bat，明明能在
    xmake 里面完成非要外部搞这些"。
  - **删掉 `build.bat`**。三个变体改成 xmake 的一个单值选项
    `--variant=web|webview|whaleui`。两个布尔开关（`--webview` / `--whaleui`）
    的毛病是它们会一起留在 `.xmake` 缓存里：`xmake f --whaleui=y` 不会清掉
    `--webview`，于是 `variant` 退化成 web —— 这就是"选 WhaleUI 出来 Web 包"。
    单值选项不可能自相矛盾。
  - **产物名带变体**：`flashsight-web[.exe]` / `-webview` / `-whaleui`，同目录
    共存，先编一个再编另一个不会覆盖。`scripts/package.py` 只接受对应变体的名字，
    找不到就报错并提示该编哪个变体，**不再退化成"随便找个 flashsight*"**——正是
    那个 fallback 让错的二进制进了发布包。
  - **切变体只重编 3 个文件**：变体宏不再项目级 `add_defines`（那会进入每个编译
    单元的命令行，改一次就全量重编）。现在拆成 `fs_ui`（共享 UI，无变体宏）+
    `fs_ui_host`（选中变体的两个后端文件，有变体宏）+ `flashsight`（main.cpp，
    有变体宏）。实测 `web ↔ webview` 只重编 `ui_server.cpp` + 2 个后端 +
    `main.cpp`，约 7 秒。
  - **webview 资源没嵌入的真因**：资源头以前写进 `src/ui/ui_assets_gen.hpp`，
    所有变体共用一个文件名，后编的变体覆盖前一个的 pack，桌面版就可能拿到 Web
    版的包（里面没有 `index.html` 的桌面形态）。现在生成头在 `build/gen/`，
    目录跨变体固定（否则 include 路径变化同样会让整个 `fs_ui` 重编），并且只在
    内容真的变化时才写盘（否则每次 build 都刷新 mtime，`ui_server.cpp` 白重编）。
  - **链接顺序**：`fs_core` 现在由 `fs_ui` 自己 `add_deps`，`mbedtls_src` 由
    `fs_core` 自己 `add_deps`。只在 exe 上并列声明三个依赖时，xmake 会把独立的
    依赖按名字排序，`fs_core` 跑到 `fs_ui` 前面 → `fs::highlight::*` 全未定义；
    mbedtls 同理。依赖边决定链接顺序，不是写在哪一行的问题。
  - **tool call 全链路**（之前只有一行 `tool.update {name, ok}`）：
    * `execute_tool` 先发 `tool.start {name, args}`，UI 立刻出现一行"运行中…"；
      工具跑完再发 `tool.update {name, ok, summary, detail}`，**原地补全同一行**
      （旧实现每次新建一个块，于是每个调用显示成"运行中"+"ok"两行）。
    * `ToolBlock` 加 `running`，未完成时显示转圈而不是对勾（还没结论就不该打勾）。
    * 落库的 assistant turn 现在存 `executed_calls_`（真正执行过的调用，含 args /
      summary / detail），不再只存模型"想调"的 `tool_calls`。重开会话时
      `turn_to_message` 优先读 `detail`，所以历史里的工具块也有内容可展开。
    * 工具结果回填时补上 `id`（模型的 tool_call id）。没有它，provider 无法把
      结果对回调用。
    * 新增两个用例：`loop_tool_call_events_and_persist`（事件 + 落库）、
      `uiapp_tool_call_start_and_complete`（start→update 原地补全、失败标红、
      detail 保留）。
  - 实测：三个变体都编译通过；资源断言脚本确认 `flashsight-web.exe` 里有
    `#fs-root` + `fs_ui.wasm` + 样式，`flashsight-webview.exe` 里有 `#fs-root` +
    样式且**没有** wasm（正确）；三个包分别产出，whaleui 包带上 `web/` 资源目录
    （WhaleUI 运行时从磁盘读 CSS，不打进 exe）。272 用例 2735 断言全过。

<71>(SOLVED): GitHub Actions 重写（变体选择、资源断言、可选 WhaleUI job）。
  - 上一批只改了本地构建，workflow 里还是 `--webview=y` / `build/web` 那套，等于
    没做。这次补齐：
  - **构建参数**改成 `--variant=web|webview`，两个变体在同一个 job 里先后编译
    （同一输出目录、切变体只重编 3 个文件，分成两个 job 会把整个项目编两遍）。
  - **可选变体**：`workflow_dispatch` 加 `variant` 输入（all/web/webview），
    迭代时只编一个。
  - **资源断言**（关键）：二进制里必须真的有 `#fs-root`、样式、以及 Web 版的
    `fs_ui.wasm`；webview 版**不能**带 wasm（带了说明用错了变体的 pack）。查找按
    `build/<plat>/<arch>/release` + 平台后缀，所以 macOS/Linux 也适用——上一版
    写死了 `windows/x64/release` 和 `.exe`。
  - **包数量断言**跟着变体选择走（之前固定要求 2 个包，单选变体时会误报）。
  - **WhaleUI 独立可选 job**：它要 clone WhaleUI、先用它自己的 xmake 编
    `libwhaleui-full`、再装 lexbor/utf8proc/SDL3/SDL3_ttf/SDL3_image 这些 xrepo
    依赖，是另一个仓库和更长的构建，所以默认跳过，用仓库变量
    `ENABLE_WHALEUI=true` 打开（`WHALEUI_REPO` 可换 fork）。`libwhaleui-full.a`
    的路径用 `find` 定位而不是写死布局；`--whaleui_dir` 也提成 xmake 选项
    （之前硬编码 `C:/aaa/WhaleUI`）。release job 用 `always()` + result 检查，
    WhaleUI 跳过时不挡发布，跑了就等它并一起挂包。
  - 顺带修掉 workflow 里过期的注释（mbedtls 早就是源码编译，不再是 xmake 包）。
  - 实测：`python -c "yaml.safe_load(...)"` 解析通过，jobs = prepare/build/
    whaleui/release，release needs 正确；把 CI 的资源断言脚本在本地跑了一遍
    （`build/windows/x64/release` 下两个 exe 全部通过）；272 用例 2735 断言全过。


<72>(SOLVED): Agent 上下文重做（三层上下文 + 中性消息 + 三形态适配）。
  - **根因**：`AgentHost::loop_for` 从未给 `AgentLoop` 填 `history.tools / env /
    system_prefix`。模型收到的是 `tools: []` 和空系统提示——"agent core 太简单"的
    真实原因不是提示词写得少，而是**根本没有提示词**（`HistoryBuilder` 只用
    `system_prefix` 拼，而它一直是空串）。上下文设计见 `doc-internal/agent-context.md`。
  - **三层**：
    1. 稳定前缀（system，逐字节稳定）：身份/工作方式/规划与工作流建议/工具索引/
       skills 与 MCP/环境/项目约定/长期偏好/思考语言。
    2. 对话（中性消息数组）：`user` / `assistant(toolCalls)` / `tool(result)`。
    3. 每轮状态块（追加在**最新一条用户消息**里）：时间/规划/清单/子代理/水位。
    易变字段不进前缀，否则前缀每轮都变、前缀缓存全废；状态块包在 `<runtime>` 里，
    落库存原文（`promptRaw`），`unwrap_user_text` 回读。
  - **规划与工作流显式建议**（用户点名要求的那段）：todo 用于"3 步以上或改多个文件"
    的一次性任务；plan 是左侧规划栏、未闭合是自动模式继续的依据；子代理的判断标准
    是**上下文成本**（要读很多文件才得出结论、而你只要结论）；工作流只在"同一条流水线
    要反复跑 / 每步要单独重试跳过"时用。每块都写"什么时候**不用**"。
  - **同时补工具**：`plan_update/get`、`subagent_spawn/list/cancel/steer`、
    `workflow_list/start`（`src/tools/orchestration_tools.*`）。此前 PlanStore /
    SubagentManager / WorkflowStore 在工具表里**一个入口都没有**，提示词等于描述
    模型没有的手。
  - **中性格式 + `provider::prompt::adapt`**：旧实现把 Anthropic 的
    `{type:"tool_result"}` 塞进 user 消息发给所有端点，`[OI]` 端点忽略它 →
    模型看不到结果 → 无限重发同一调用（直到 20 次上限）。现在按
    `[OI]`（`role:"tool"` + `tool_call_id`）/ Anthropic（`tool_use`/`tool_result`
    blocks + 顶层 `system`）/ Responses（`function_call_output`）分别渲染；
    Anthropic 额外修"相邻同角色合并""首条必须是 user"。
  - **整组调用**：一条回复里多个 toolCall 逐个执行，每个结果单独一条 `tool` 消息
    （带 call id）。审批门按**整组**保存（`gate_messages_/gate_calls_/gate_index_`），
    批准后继续执行剩余调用——旧实现只记一个 pending，组里其余调用直接丢掉。
    拒绝时把 `permission-denied` 回填（不静默）。参数 JSON 解析失败**不执行**
    （否则 `write_file` 的坏参数会变成"清空文件"）。
  - **边界**：单轮往返 60、连续相同调用 3、自动续跑 12；触发就结束本轮并在回答里
    注明原因（不再返回错误让 UI 以为 send 失败）。自动续跑前把模型上一条回答放进
    历史，否则看起来像"用户凭空说继续"。
  - **历史**：预算从**最新往回**累加（旧实现正序累加 + 一超预算就 `break`，把最新的
    对话全丢了）；assistant 消息后紧跟它的 tool 结果；折叠摘要带 `prior_summary` 与
    `keyPoints`；工具正文按字符边界截到 8000。
  - **子代理真正执行**：`AgentHost::run_subagent` 起独立 `AgentLoop`——不读父会话历史、
    不落库、只读预置代理只拿只读工具、权限不超过父；worker 线程跑完
    `complete_with_summary` + `subagent.done`；宿主析构 join 所有线程（线程捕获 this）。
  - **UTF-8 坑**（花了最久）：`find_first_of("。.\n")` 匹配的是**字节集合**，`。` 是
    `E3 80 82`，其中 `0x80/0x82` 也是别的汉字的续字节 → 从汉字中间切开 → 非法
    UTF-8 → `json::dump()` 抛 `type_error 316`，整轮崩（`m3_manual_e2e_persist` 就是
    这么挂的）。同理 `substr(0, 4000)`。新增 `common/fs_utf8`（`truncate` 只切字符
    边界、`sanitize` 兜底、`valid` 自检），所有截断改走它；多字节分隔符一律 `find`。
  - 新增 25 个用例：`tests/test_context.cpp`（前缀稳定性、规划/工作流建议存在、
    runtime 不污染前缀、三形态适配、结构修复、参数归一化、历史预算/遗忘/keyPoints）、
    `tests/test_loop_flow.cpp`（整组调用、审批中断续跑、拒绝回填、坏参数不执行、
    重复熔断、自动续跑、宿主接线真的带工具表和非空系统提示）。
  - 实测：297 用例 2891 断言全过；`--variant=web` 与 `--variant=webview` 都编译通过。

<73>(SOLVED): 水位驱动压缩 + edit_file 协议 + 用量口径 + 按步骤派子代理。
  - 用户反馈四条：① 压缩应该看水位（设置里有阈值），不是固定预算；② 侧栏用量
    只给相对水位和输出 token，缺缓存命中、单轮/总成本、钱/额度/未知三态；
    ③ 子代理应该拆解单轮任务、每步单独派 step 子代理，工作过程可显示但不进历史；
    ④ 文件工具应该是 edit_file（行号替换 + 哈希），不是 diffWrite 文本替换；
    另外还要求确认"模型记忆对齐（哈希 + 二次验证）"和"read 防大文件"。
  - **压缩改成水位驱动**：`HistoryBuilder::should_compress()` 是独立可测的决策函数，
    三种触发——水位（`history.compressAtPct` 默认 80，读上一次请求的 prompt tokens /
    模型窗口）、预算（`history.tokenBudget` 默认 32000，窗口未知时兜底）、强制
    （用户点压缩）。**关键修复**：旧实现只是把预算调小，短会话仍然放得下 →
    "点了压缩什么都没发生"。现在决定压缩就必然保留 `keepTurns` 条并折叠其余。
  - **水位跨重启有效**：水位从用量日志读回（`usage.session.lastIn`），不是只存在
    内存里；`deps.last_prompt_tokens` 注入宿主，重开会话第一轮就能正确判断。
  - **edit_file 协议**（`edit_file`，移除 `file.diffWrite`）：读返回整文件 `hash`
    （xxh64 前 12 位），编辑带 `hash` + 每段 `expect`。两道校验各防一种错：
    hash 不符 = 文件被改过（返回 `currentHash` + `stale`）；expect 不符 = 行号对但
    内容不是模型以为的（返回 `actual`）。多段编辑按原始行号解释、从下往上应用；
    区间重叠拒绝；行号越界拒绝（不静默追加）；`newText` 空 = 删行。
  - **行尾与末尾换行**：`split_text` 显式记录 `final_newline`。测试抓到真 bug——
    旧实现每次编辑都会吃掉文件末尾的换行（POSIX 违规 + 整文件 diff）。CRLF 文件
    编辑后仍是 CRLF；插入的多行按文件自己的 EOL 拼接。
  - **read 守卫**：>32MB 拒绝、>1MB 需显式 offset/limit、单行 >4000 字符截断并标记、
    前 4KB 含 NUL 判二进制。`find_files` 的 grep 也跳过超大文件。
  - **用量口径**：`UsageSummary` 加 `cache_hit_ratio`（分母只算**上报过 cache**的行，
    未上报 = -1 而不是 0%）、`cost_kind_by_provider`（一个会话可同时有付费与套餐
    供应商，加在一起是错的）、`recent[]` 最近 50 行（UI 据此算"本轮"）。
    侧栏显示：水位百分比 + 绝对值、缓存命中率 + 命中 token 数、本轮 tokens/成本、
    会话累计成本、调用次数；超阈值直接给"压缩上下文"按钮。设置页补阈值档位、
    保留轮数、token 上限、"立即压缩"，并把键名从不存在的 `history.compressPct`
    统一成 `history.compressAtPct`。
  - **按步骤派子代理**：前缀的规划节明确写"清单建好后逐项 subagent_spawn
    （kind=complete-step、step=标题、todoId=id）"，并写依赖顺序。子代理上下文加
    `subagent_step`："只做这一项，做不了说明原因，不要扩大范围"——不再要求它
    重新规划整个任务。`subagent_steps_` 记录 step↔todo 映射，完成时自动
    `todo_check`（进度条不会和现实漂移）。
  - **上下文隔离**：子代理的 `stream.delta` / `tool.*` 事件现在带
    `sid=父会话` + `subagentId` + `fromSubagent`：主界面能显示"正在做什么"，
    但这些内容**不进 full.json、不进父会话 messages**。这是"工作过程可显示但不
    进入历史"的实现。
  - **unattended 模式**：子代理遇到需要批准的动作直接拒绝并回填
    `howToProceed`，不会挂在 `waiting` 等一个永远没人按的按钮。
  - 新增 `src/common/fs_hash.*`（xxh64，无依赖，附已知向量测试）、
    `tests/test_agent_v2.cpp`（19 个用例）。总计 318 用例 3008 断言全过；
    web / webview 两个变体编译通过。
  - **踩坑**：用 PowerShell 的 `Get-Content`/`Set-Content` 批量改 `.cpp` 会把 UTF-8
    中文按 ANSI 读写、整文件乱码（且 `-replace` 会把 `\n` 吞进注释）。已
    `git checkout` 回滚并改用 Edit 工具。以后批量替换一律走 Edit/脚本文件，
    不要用 PowerShell 文本管道改源码。

<74>(SOLVED): "任务已中断 http 400" 的两个真根因（工具名非法 + 错误体被丢）。
  - 用户报告：接上真 provider 后每轮都失败，UI 只显示"任务已中断 http 400"。
  - **根因一：工具名带点号**。OpenAI 与 Anthropic 都要求工具名匹配
    `^[a-zA-Z0-9_-]{1,64}$`。上一轮我把工具表真正发给了模型（之前根本没发），
    于是 `file.read` / `plan.update` 这类点号名字让**每个请求**在到达模型前就被
    400 拒绝。全部改成下划线风格：`read_file` / `write_file` / `edit_file` /
    `list_dir` / `find_files` / `remove_file` / `terminal_exec` / `fetch_url` /
    `todo_add` / `plan_update` / `subagent_spawn` / `load_skill` …（共 37 个）。
  - **两层防护**：`ToolRegistry::register_tool` 现在调用 `wire_safe_tool_name()`
    规范化（`.`/`/` → `_`、超长截断），并把 `ToolDef.name` 同步成规范名——
    注册表键 / schema / 模型回调三者永远一致；以后新增或导入的工具写错也不会
    再让整个请求 400。新增 `tests/test_wire_names.cpp` 遍历真实注册表断言合法性。
  - **根因二：错误体被丢掉**。`provider_client` 把非 2xx 压成 `"http 400"`，
    而端点其实回了 `tools[2].function.name: does not match pattern ...`。
    现在 `http_error_text()` 解析 `{"error":{"message":...}}`（Anthropic 同形态）、
    非 JSON 用原文、压成单行截 400 字符。这条让以后所有 provider 错误都能直接定位。
  - **注意区分**：事件名（`subagent.done`）、FsApi 路由（`plan.update`）、
    设置键（`history.compressAtPct`）不进请求体，保持点号不变。
  - **批量重命名踩的坑**：`scripts/rename_tools.py` 做纯字符串替换，把
    `subagent.steered`（事件）、`plan_update` 的 FsApi 路由、`deps.plan_update`
    也一起改了。已用 `scripts/fix_event_names.py` 反向修回，并逐条确认剩下的
    点号只出现在事件名/路由/注释里。
  - 顺带修：`file_regex` 曾出现在只读子代理的白名单里（它会写文件），已移除；
    新增 `set_subagent_tool_probe` / `wait_subagents` 测试钩子，
    用例 `read_only_agent_has_no_write_tools` 断言只读代理拿不到
    `write_file`/`edit_file`/`remove_file`/`file_regex`/`terminal_*`。
  - 端到端验证：写了个临时探针打印**真实请求体**（37 工具、三种 provider 形态），
    确认非法名 0 个、`assistant.tool_calls[0].function.name` 与
    `tool.message.tool_call_id` 正确配对、整体是合法 UTF-8。
  - 325 用例 3313 断言全过；web / webview 两个变体编译通过。
  - 详见 doc-internal/agent-context.md §2.5.1（命名表）与 §10.3（错误信息）。

<75>(SOLVED): 图片/搜索真接线 + 桌面端流式卡死（低级错误）。
  - 用户实测三条：① image 两个函数没接线（截图只测过 Win32，要求全平台）；
    ② search 也没接线；③ Webview 下对话不是流式、直接卡死。
  - **卡死根因（最严重）**：`NativeBackend` 没重写 `IUiBackend::call_async`，
    用的是接口默认实现——**同步调用**。于是桌面版每次 `session.send` 都在 UI 线程
    上跑完整个 turn（模型往返 + 所有工具 + 下一轮），窗口冻住、delta 全排队、
    什么都不流式。浏览器版走 HttpBackend 天生异步，所以从没暴露。
    修法：`call_async` 入 FIFO 队列，由**单个**工作线程执行；`FsApi` 不是线程安全
    的（持有 map），多线程会踩数据，单线程既安全又不挡 UI。结果与事件共用一条
    队列，`poll()` 在 UI 线程交付（webview 的 ticker 已经用 `webview_dispatch`
    跳到 UI 线程，所以 DOM 调用仍只在 UI 线程）。析构停线程。
  - **图片链路**：
    * 截图：`common/fs_screen` 三平台实现——Windows GDI `BitBlt`、macOS
      CoreGraphics、Linux X11 `XGetImage`；Wayland 明确报不支持（没有可移植 API）。
    * 解码：`common/fs_image` 自写 PNG 解码（stored/fixed/dynamic Huffman inflate）
      + 32 位 BMP；JPEG/GIF/WebP 不重编码、原字节放行。
    * 缩放/编码：双线性（只缩不放，长边受限）+ 自写 PNG 编码。
    * **模型能看见**：工具返回 `dataUri`，loop 把它从 JSON 里取出变成中性 `parts`
      块，adapter 渲染成 OpenAI `image_url` / Anthropic `image` base64 source。
      只返回路径的话模型根本看不到图——这是"接线"的关键一步。
    * Windows 非交互会话（锁屏/未连接 RDP/服务）BitBlt "成功"但全黑：现在检测
      全黑并报错说明原因，不把黑图喂给视觉模型。
  - **搜索链路**：`tools/search_backend` 支持 tavily/brave/searxng/custom，
    各自请求形态与响应解析；缺 key、未配置、HTTP 错误都给人话；空结果不算错。
    原来 `search_web` 把 `{search:true}` 塞进 `net_fetch`，而那个回调只读 `url`
    → 每次 "empty url"。现在有独立的 `web_search` 回调，设置页在"权限-网络"下
    可选 provider 并填 key/endpoint。
  - 新增 21 个用例（`test_vision_search.cpp` 17 + `test_native_backend.cpp` 4）：
    PNG 往返/坏数据/缩放/base64、read_image 返回 dataUri、screenshot 不再是
    not-supported、电脑控制开关、四种搜索请求形态与解析、异步不阻塞/串行执行。
  - 实测：本机截图能拿到 2048x1280 的真实位图，PNG 往返像素完全一致（用探针验证，
    探针已删）。346 用例 3419 断言全过；web / webview 编译通过。
  - 踩坑：PowerShell 批量改 .cpp 会破坏 UTF-8（上次已记）；这次改用 Python 脚本
    （显式 utf-8）做批量替换，一次性脚本用完即删。

<76>(SOLVED): 桌面端卡死重做（多线程）+ 截图改 DXGI 优先 + 图片改用 stb。
  - 用户指出三条：① Webview 的修法不靠谱（"实在不行开多线程也不是不行"）；
    ② 截图 BitBlt 是旧做法，1909+ 主流是 Modern API（Desktop Duplication），
    建议优先它、全黑时 fallback BitBlt；③ 图片建议用 stb_image + stb_image_resize2。
  - **卡死重做**：上一版用"单个工作线程串行"，能跑但一个长 turn 期间所有其他
    调用（打开面板、点停止）都得排队，仍然不对。现在改成：
    * **工作线程池**（按需增长到 4）：turn 占一个，UI 调用还有别的可用；
    * `FsApi` 非线程安全（持有 map），所以在 UI→后端边界 `UiApi::call` 取
      **一把递归锁**，130 个路由一次覆盖，以后新增的自动受保护；
    * `session.send` / `tool.invoke` **不**在这把锁里（前者整个 turn、后者可能跑
      一条终端命令），各自只锁短前奏，否则等于回到"冻住"；
    * **嵌套调用内联**：turn 回调里再调后端时，`thread_local` 标记识别后直接执行，
      排队会等自己→必然死锁（新增用例 `native_backend_nested_call_does_not_deadlock`）；
    * `AgentLoop` 的 `state_`/`suspend_requested_`/`saved_valid_`/
      `compress_requested_`/`context_limit` 改 `atomic`——UI 每帧读它们画状态点，
      原来是数据竞争。
  - **截图改 DXGI 优先**：`Desktop Duplication`（D3D11 + DXGI）读的是 DWM 合成后的
    表面，锁屏/未连接 RDP/多 GPU/HDR 下 BitBlt 返回全黑而它正常；Duplication 不可用
    （无适配器、被占用）才回落 GDI。`Capture::backend` 报告实际用了哪个。
    实测本机：Duplication 在这台机器上不可用（返回失败），自动落到 GDI 并成功
    截到 2048x1280 —— fallback 路径验证过了。
  - **图片改用 stb**：`3rdparty/stb`（三个单头，MIT/公有领域，已加进
    `scripts/fetch_3rdparty.py`）。`stb_image` 解 PNG/JPEG/GIF/BMP/TGA/PSD/HDR
    （WebP 无解码器，明确报错并给真实尺寸）；`stb_image_resize2` 做 sRGB 感知缩放
    （文字缩到 1600px 仍可读，朴素双线性做不到）；`stb_image_write` 出真压缩 PNG
    （自写的 stored-deflate 会把一张截图撑到几 MB）。解码前用 `stbi_info` 读头部，
    按 `kMaxPixels` 拦解压炸弹。
  - xmake：`3rdparty/stb` 加入 include；Windows 链 `d3d11`/`dxgi`；Linux 链 `X11`。
  - 347 用例 3421 断言全过；web / webview 编译通过。
  - 详见 current.md <76>、doc-internal/agent-context.md §10.4、thirdparty.md。

<77>(SOLVED): ask 静默失败 + 自动轮无预告/无小圆点/混进历史 + 失败轮丢用户输入。
  - 用户实测五条：① ask 不挂起、静默失败；② 自动轮没有任何预告；③ 主页没有
    "后台运行"紫色小圆点；④ 自动轮完全没有界面显示、稀里糊涂；⑤ 输入提示失败但
    后台跑起来了，回来输出在、输入没了；⑥ 冒出 "tool round limit reached (60)"。
  - **ask 静默失败（根因）**：工具自己发 `ask.prompt` 然后返回 `asked:true`，
    于是模型以为"用户已经回答了"，本轮继续跑，用户的选择从未回填。
    现在：工具只返回 `awaiting_user`，**loop 像权限门一样挂起本轮**（state=waiting），
    用户选择由 `answer_ask` 作为该次工具调用的结果回填后 `run_loop` 续跑。
    过期 askId 被拒绝（不会用旧对话框的选择去回答当前问题）。
  - **ask 界面**：原来复用权限确认框（只有"允许/拒绝"两个按钮），选项根本没显示。
    现在单独的 ask 弹窗：问题 + 选项列表（标"推荐"）+ 可选自由输入；关闭弹窗也会
    回填"(dismissed)"，不会把本轮永远挂着。
  - **自动轮预告**：`auto.next` 事件带 round/maxRounds，UI 弹 toast「自动继续（第 N
    轮）：原因」。原来这个事件 UI 根本没处理，所以"没有任何预告"。
  - **紫色小圆点**：`session_dot` 新增 auto_round 参数——有自动轮时走 background
    分支（紫）；`session.list` 也回 `autoRound`，主页/项目卡片不用打开标签页就能看到。
    原来自动轮既不是 running 也不是 terminal，所以显示成"空闲"。
  - **自动轮混进历史**：自动轮的 nudge 文本被当成用户输入存成 user turn，重载后
    渲染成用户气泡、和上一轮粘在一起。现在存档带 `autoRound` + `system:true`，
    `turn_to_message` 把它转成 system 分隔条（带左右横线 + "自动继续 · 第 N 轮"），
    加载时也不再被 `role == user/assistant` 过滤掉。
  - **失败轮丢输入**：`run_loop` 首发失败时只保存了断点、**从不落库 user turn**，
    所以重开会话只看到后来后台跑出来的回答、没有当初的提问。现在失败时先
    `persist_user_turn_only()` 再返回。
  - **"tool round limit reached (60)"**：这是我上一轮自己加的安全上限，用户没设计过。
    现在：默认值 60 → 200（真正的安全网），措辞改成"本轮工具调用达到上限，已暂停，
    可以继续"，事件从 `loop.interrupted`（会弹"失败，可继续"）改成 `loop.paused`
    （只提示，不冒充失败）。
  - 新增 6 个用例（`test_ui_flow.cpp`）：ask 挂起+回填、过期 askId 拒绝、自动轮
    带轮数预告、自动轮存档标记、失败轮仍存输入、工具上限是暂停不是中断。
  - 353 用例 3455 断言全过；web / webview 编译通过。

<78>(SOLVED): ask 面板重做（覆盖输入框/多问题/手动发送）+ SYSTEM 前缀 + round limit 自动开新轮 + 成本估算 + markdown 表格/引用/链接。
  - 用户反馈六条：① ask 弹窗效果差，分辨不出选项，应覆盖输入框区域、显式给出选项、
    手动点击发送；需要标题/问题正文/预置选项+自定义，支持一次多个提问并显示 1/x；
    ② 自动发送的消息要带 `SYSTEM:` 前缀；与用户消息放一起可以，但应是系统消息不是气泡；
    ③ round limit 有道理，但实测 1h8m/93 次工具调用，200 次并非不可达——建议 200 次后
    自动开新的一轮，并要求模型审视工作（**指明是工具自动发的通知**，否则会误导模型
    提前结束超长任务）；④ 成本统计仍无效果（DeepSeek 官方 API 有金额数据）；
    ⑤ markdown 没有引用/TIP/WARNING/表格；⑥ 链接未测，指向文件的应在侧边栏打开。
  - **ask 面板重做**：从浮层对话框改为**占据输入框区域**的 `ask-panel`——标题（含
    `1/3` 计数）+ 问题正文 + 选项行（单选圆点 + 推荐标记）+ 自定义输入 + 发送按钮。
    点选项只是**选中**，必须点"发送回答"才提交（用户明确要求手动发送，避免误点）。
    多个提问按顺序展示，"下一个"切换；一次提交所有已答项，后端收齐才续跑。
  - **多问题**：`run_tool_group` 把同一回复里的所有 ask 调用合并成一个 `ask.prompt`
    （带 `asks[]`），而不是逐条弹出；`answer_ask` 按 askId 收集答案，**全部答完**
    才把每个结果依次回填并 `run_loop`。
  - **SYSTEM 前缀**：自动续跑的 nudge 改为 `SYSTEM: ...`，存档仍是 user turn 但带
    `autoRound`+`system:true`，渲染成系统分隔条（不是气泡）。
  - **round limit 改为自动开新轮**：达到上限不再"暂停等用户"，而是结束本轮、
    发 `auto.next`、开新一轮；nudge 明确写"这是**系统自动切分**，不是用户要求停止，
    也不代表任务完成，请先审视已完成的工作再继续"。默认上限 60→200 保留作为安全网。
  - **成本**：`ModelCapabilities` 加三个单价（每百万 token：in/out/cache）。
    `ProviderClient` 按优先级取：端点回报的 `cost` > `usage.cost` > **单价×token 估算**
    （标 `estimated`）> quota > unknown。缓存字段兼容 `prompt_cache_hit_tokens`
    （DeepSeek）与 `prompt_tokens_details.cached_tokens`。UI 里 `estimated` 显示
    `≈¥x（估算）`，绝不冒充真实账单；预置表补了 deepseek/gemini 的单价。
  - **markdown**：新增 blockquote（`>`，带左侧竖线）、`---` 分隔线、**表格**
    （表头+对齐行+单元格）、`[text](url)` 链接（锚点带 `data-href` 不带真实 href，
    点击由应用决定去向）。
  - **链接打开**：新增 `link.open` 路由 + `FsApi::open_link`：http(s) 交给系统浏览器
    （ShellExecute/xdg-open，白名单协议）；项目内相对路径返回给 UI 打开**侧边栏预览**；
    `file://` 与自定义协议拒绝。两个 DOM 适配器都加了 `data-href` 的点击值提升。
  - 358 用例 3489 断言全过；web / webview 编译通过。

<79>(SOLVED): 子代理 403 + 压缩设置不生效 + MCP/Skill 混淆 + 工具结果要投影 + i18n 补齐 + 快捷键接线 + 弹窗失焦 + 滚动状态。
  - 用户反馈八条：① 子代理被 403，且 step 该显示在主会话、其他该进侧栏；② 设置-模型偏好-压缩里
    的阈值选项"没有意义"；③ MCP 有误（gsd 在 MCP 里启用但指令跑到 Skill）；④ 上下文占用太多，
    工具结果应在下次 API 调用时只给摘要（去掉无用键、紧凑 json）；⑤ placeholder / 侧栏标签说明
    大多没 i18n；⑥ 快捷键尤其是 Enter 相关"就是摆设"；⑦ 所有弹窗都不能失焦自动关闭（右键菜单、
    选择模型）；⑧ 组件状态管理没做好：切换会话不保存滚动状态。
  - **子代理 403 的真根因**：`AgentHost::send` 用 `loop->context().model` 建上下文，而 context 在
    新 loop 上是空的（`set_context` 才填）——于是 `ctx.model` 为空，`session_ctx_` 里存的就是空模型，
    子代理 spawn 时 seed 也是空 → `FsApi` 的 provider 路由"认不出模型就回退到第一个启用的连接"
    → DeepSeek 模型被发到 Anthropic 端点，403。改为读 `loop->model()`（新增访问器），并加
    `AgentLoop::model()`/`live_state()` 的注释说明为什么不能读 context。
  - **压缩设置"没有意义"**：`history.*` 阈值只在 loop 创建时读一次，改设置要重启才生效。
    `AgentHost::send` 现在每轮重读 `recentTurns/tokenBudget/compressAtPct/keepTurns` 并
    `set_history_options`（该 setter 保留 tools，避免把工具表清空）。
  - **MCP/Skill 混淆**：`mcp_detect` 探测的是 `~/.claude/skills`、`~/.claude/commands` 这些
    **Skill 目录**，把每个子目录注册成一条 MCP 条目——用户配置里 72 条 `mcp.entries` 全是
    `gsd-*` 这类 skill 目录，且和 `skills.entries` 完全重复。现在：
    * `mcp_detect` 读**服务器定义**（`mcpServers` / `mcp.servers` / `servers`），新增
      `McpRegistry::import_config_file`；
    * 新增 `skill_detect`（skill 目录探测）+ `skill.detect` 路由，Skill 页的"自动探测"
      不再调用 `mcp_detect`；
    * 条目带 `kind`（server|dir）与 `server`（command/url/args），MCP 卡片显示真实命令行；
    * **启动时迁移**：旧表里 `dir` 落在 skills 目录下的条目，导入 skill 注册表并从 MCP 表移除
      （实测把用户的 72 条 MCP 条目清成 0，skills 仍是 72 条）。
  - **工具结果投影（上下文优化）**：新增 `HistoryBuilder::project_tool_result`——按工具保留
    "能继续干活"的字段（路径/行号/hash/exitCode/todo 项/失败原因），丢掉 bulk（read_file 正文
    压到 900 字符、edit 的 before/after 只留行区间与条数、list_dir 只留名字样本、终端取 **尾部**、
    图片不留 dataUri）。老格式（只有 `detail` 文本）先尝试解析成 JSON，失败则保留截断文本，
    不会把结果整个丢掉。同一轮内（模型还要据此干活）保留原文，只做 16KB 上限。
  - **i18n 补齐**：写了三个静态审计脚本（`temp/i18n_*.ps1`）扫出 tr() 键缺 en 条目 124 条、
    只写中文没套 tr() 的可见文案 7 处。补齐后剩 1 条（头文件里的注释）。特别修了侧栏面板
    标题/说明（`side_title` 现在过 tr()）与 `kSideKinds` 的描述、Web 服务页、压缩页、快捷键页。
  - **快捷键接线**：`bind_global_key` 的值从裸键名改成带修饰符的 `"Ctrl+W"`（wasm/webview/whale
    三个适配器都改；WhaleUI 新增 document 级 keydown 监听，window 回调仅在无它时兜底，
    避免双触发）。`render()` 里按设置表匹配"关闭标签页/新建标签页"；"发送消息"含 Ctrl 时
    输入框切到 Ctrl+Enter 发送（否则尊重会话自己的设置）。快捷键页的说明文字也不再写
    "桌面端接入后生效"。
  - **弹窗失焦关闭**：`comp::modal` 的 backdrop 点击保留，另外在 shell 里对 `modal:*` 绑定
    `bind_outside_click`；**权限门的"关闭"是拒绝**（loop 正停在那里，不表态会挂住整轮），
    confirm 弹窗则只清状态。Esc 的分支顺序改为 模态 > picker > 右键菜单 > 菜单，且模态分支
    同样处理权限门拒绝。同时修了两个适配器里 `outside` 的父容器豁免——原来豁免整个
    parent（`host.contains(t)`），点 popup 的兄弟节点都不关闭，看起来"关不掉"。
  - **滚动状态**：webview 适配器 `set_scroll_top` 现在**同步回写**镜像（它的读是异步的，
    原来切走时读到的是旧值，于是保存的是旧位置）；滚动事件里也顺手更新 `hist_scroll`，
    不再只在切标签那一刻读一次。新增用例覆盖（切走再回来 offset 保持）。
  - **子代理事件分流的落地**：`complete-step` 的 stream/tool 事件进主会话，其余（research/
    explore/check-files）只进侧栏——宿主在每个事件上带 `subagentKind`，`apply_session_event`
    据此过滤。新增用例覆盖两个方向。
  - **ask 自定义行**：补 `.ask-option.custom` / `.ask-custom-wrap` 样式（与其他选项同形、
    选中后在场内展开输入框）。
  - **测试**：新增 6 用例（工具结果投影、弹窗失焦、confirm 不失焦误删、会话滚动 offset、
    快捷键改 Enter 行为、子代理事件分流）。363 用例 3506 断言全过；web / webview 编译通过；
    实测 web 服务起来后 `skill.detect` 能发现 skills 目录、`mcp.detect` 不再返回 skill 目录。

<80>(SOLVED): 子代理线程栈泄漏 + 弹窗失焦（单回调覆盖/僵尸监听）+ 规划不实时/可乱改 +
  子代理看不到内容/引导是摆设/follow-up + 历史分片 + 顶栏主页驻留。
  - 用户反馈：① plan_update 和 todo_add 看着像一回事，实测规划没更新面板、面板里手动添加
    是画蛇添足、完成状态和"据此判断继续"都没落地；② 子代理侧栏只能看到"在运行"和"引导"，
    看不到在跑什么，引导按钮不知道在干什么，工作过程窗口太小、点不进历史；③ agent 工具其实
    可以在返回后继续给子代理引导；④ **疑似严重内存泄漏**（webview 无响应，机器险些重启）；
    ⑤ 弹窗（右键菜单、设置里的模型选择）依然无法失焦关闭；⑥ 顶栏标签多了以后访问不到，
    应做成主页驻留 + 其余滚动。
  - **内存泄漏（最要紧）**：`subagent_threads_` 是个只增不减的 `vector<std::thread>`——
    线裎跑完但没 join 时**仍占着线程栈**（Windows 每个保留 MB 级地址空间），而它只在
    `wait_subagents()`（析构）里被 swap 出去。于是**每个子代理都泄漏一份栈**，长时间会话
    必炸。改为 `SubagentThread{id, thread}`，新增 `reap_subagent_threads()`：按**记录状态**
    判定完成 → 移出 → 在**不持锁**的情况下 join（线程最后一步是 emit，持锁 join 会死锁），
    在每次 spawn 前与 `on_timer_tick` 调用；并用 `thread_local g_in_subagent_thread` 防止
    嵌套 spawn 时 join 自己的祖先/自己。
  - **弹窗失焦（根因有三层）**：
    * 适配器只保留**一个** outside 回调（`outside_cb_` / `outside_cbs().clear()`），多个弹窗
      同时存在时后者覆盖前者；而且**文档级监听器不会随节点移除**，所以旧弹窗的监听器会去调
      **当前**那个回调 → 关错对象/关不掉。改成**按 popup 节点 keyed 的回调表**（webview/wasm/
      whale 三个适配器），并在 JS 侧加 `isConnected` 判断（已脱离 DOM 的弹窗不再生效）。
    * 右键菜单用**缓存的 `ctx_menu_node_`** 绑定，菜单重建后指向已脱离的节点 → 绑定到空气。
      改为渲染时按 key 查 `ctx-menu`。
    * 权限门/确认框的失焦关闭保持（关闭=拒绝，否则挂住整轮）。
  - **规划**：宿主一直发 `plan.update`，**UI 从没监听**（面板只在点按钮时 `plan.get`），所以
    模型写好的规划看不见 → 加事件处理（后台会话落到 stash 视图）。`plan_steps` 从
    `vector<string>` 改为带 `status/note` 的结构（兼容老的 `done` 布尔与裸字符串），面板显示
    状态标记 + `已完成 n/总数`（**与 `should_auto_continue` 同一条件**）。**删掉手动增删改**：
    规划是模型的工作状态，手改等于写一份模型没同意过的树；保留评审（通过/驳回/改）。
  - **子代理面板**：运行中显示 `live_text`（带 `subagentId` 的 delta/tool.start 累积、只留
    尾部 2KB）；转录里补上 tool call 名字（以前工具行整段消失）；「引导」改成**输入框 + 发送**：
    运行中 → `subagent.steer`，**已结束 → `subagent.followUp`**（同一 id、带原转录再跑，
    同步返回新摘要）。为此加 `SubagentManager::reopen_for_follow_up/transcript_for`、
    `AgentHost::subagent_follow_up`、工具 `subagent_follow_up`、路由 `subagent.followUp`，
    以及 `HistoryBuilder::Options::seed_messages`（把上一轮的转录作为历史回放）。
    工作过程展开区从 `min(10*1.55em+3em, 50vh)` 提到 `min(28em, 70vh)`，工具详情 220px → 60vh。
  - **历史分片**：`full.json` 原来**每追加一轮就整份重写**（O(n)/轮，6 轮就 90 KB）。
    改为 `turns/NNN.json`（每片 16 轮）+ full.json 只留 meta/settings/ops；`load_full()` 仍
    返回完整 `turns`（读时拼接），**上层零改动**。所有改写历史的路径走 `save_full()`（`forget`
    原本直接写 full.json，被这次改动抓出来修正）；rollback/trim 后删多余分片（否则被裁的轮次
    会复活）。**兼容旧格式**：老会话内联 turns 照读，下次追加自动迁移；判定用"存在分片文件"
    而非"存在 turns/ 目录"（后者会把老文件误判、丢掉全部历史——用例抓到了）。
  - **顶栏标签**：`tab_strip` 拆成 `tabs-fixed`（主页）+ `tabs-scroll`（会话标签 + "+"），
    开再多会话主页也不会被推出屏幕。
  - **测试**：新增 3 用例（规划实时更新、规划只读+评审、分片读写+回滚删片、老格式兼容），
    改 3 个受影响的旧用例。366 用例 3553 断言全过；web / webview 编译通过。

<81>(SOLVED): 急性卡死（定时器持锁 join + 子代理 delta 每帧重绘）+ spawn wait 无效 +
  list 撑爆 + 子代理记录进独立标签 + 过程说明进工作过程。
  - 用户纠正：卡死**不是慢性泄漏**，而是"生成子代理后几秒内"发生；中途输出不显示；
    子代理应可点卡片跳新标签看历史（工作过程里的"生成子代理"也要能点），否则
    **子代理一结束就永久看不到**，是设计缺陷。附 AI 实测：`spawn(wait=true)` 不阻塞、
    无 summary；`subagent_list` 3 个代理就被截断（每项内嵌 ~10KB system prompt）。
  - **卡死根因（两个，都在子代理路径上）**：
    1. **定时器持锁 join**：`on_timer_tick()` 持 host 锁调 `reap_subagent_threads()`，
       而它 join 已完成的子代理线程；子代理线程最后一步是 `emit()`（要同一把锁）
       → 定时器等线程退出、线程等锁。tick 是 60ms 周期，所以表现为"派完子代理几秒后
       整个应用冻住"。改为 **reap 在取锁之前**（并写进函数注释当契约）。
    2. **子代理 delta 每帧全量重绘**：上一轮我加的"侧栏实时内容"直接 `render()`，
       子代理一开口每秒上百个 delta → 事件循环被饿死（主对话早就用 25ms 下限节流，
       这条路径漏了）。改成只置 `subagent_stream_dirty_`，由 `pump()` 按人类节奏画。
    3. 附带修掉**数据竞争**：子代理线程的收尾块（`complete_with_summary` +
       `subagent_steps_` + `todo_op`）没有 host 锁，而主线程并发读同一批 `std::map`。
  - **`spawn(wait=true)`**：参数以前完全没读。现在**轮询记录状态**直到报回（不是 join
    线程，否则重现死锁），返回值带 `summary`。
  - **`subagent_list` 撑爆**：`to_json()` 不再带 `full`（转录里每条消息都含 system prompt
    副本），只给 `transcriptMessages` 条数；完整记录走新路由 `subagent.detail`。
  - **子代理记录可看**：侧栏卡片**可点击** → 打开独立标签 `sa:<id>`
    （`SessionTab::kind="subagent"` + `build_subagent_page`），页面显示任务/摘要/耗时/
    **完整转录**（不折叠不截断）；`subagent.done` 会刷新已打开的标签。工作过程里
    `subagent_spawn`/`follow_up` 的行从结果里解析出 `id`，加「打开记录」按钮
    → 同一个标签。这解决了"一结束就永久看不到"。
  - **中途输出（过程说明）**：模型在工具调用之间的旁白既不是答案、也没被保存——以前
    只有最后一轮的文本落库，中间说明关掉会话就消失，流式时还被粘在答案气泡里。
    现在每轮发 `assistant.interim`，UI 归入工作过程的"过程说明"一节，并随 turn 落库
    （`turn.interim`），折叠标题也标出 `N 段过程说明`。
  - **高度**：工作过程折叠区 28em/70vh → **78vh**，单次调用详情 60vh → **65vh**
    （工作过程内 70vh）。
  - **cancel** 现在区分"取消了运行中的"与"本来就没在运行"（返回 `cancelled:true/false`）。
  - **测试**：新增 3 用例（tick 与完成中的子代理不死锁——用循环 tick 覆盖"已标记完成、
    线程未退出"的窗口；list 不带转录 + detail 有；interim 事件与落库）。
    369 用例 3572 断言全过；web / webview 编译通过。

<82>(SOLVED): 卡死真因（follow_up 持锁跑 loop + collect_runtime 无锁读）+ 工作过程不显示
  （纯工具轮次没有 assistant 消息）+ 子代理记录改回侧栏 + 记录表上限。
  - 用户补充的关键线索：**"spawn 第二个子代理才会出现"**、完全卡死后程序自己退出
    （既是死锁又像内存问题）。上一轮我只修了 tick 持锁 join，没修到这条路径。
  - **卡死真因 1：`subagent_follow_up` 持着 host 锁跑整轮 agent loop。** 它 emit（要锁）、
    还可能嵌套 spawn（worker 要锁）——第二个子代理一出现就硬锁死。改为锁内只做快照
    （kind/parent/transcript/seed + `reopen_for_follow_up`），**释放后再跑**。
  - **卡死真因 2：`collect_runtime()` 无锁读 `records_` / `todos_`**，而子代理线程在并发写。
    无锁读 `std::map` 是数据竞争：会 corrupt（崩）或自旋（卡）。这解释了"只有第二个
    子代理才出现"——单个代理时没有并发写者。现在持 host 锁。
  - **真因 3（内存）**：`records_` 永不清理，每条记录带整份转录、转录里每条消息又带一份
    system prompt → 每会话几 MB。新增 `prune_finished(sid, 40)`，tick 里按会话调用，
    只删已结束的（运行中永不删）。
  - **工作过程依旧不显示的真因**：模型常常**只发工具调用、不发文本**，于是没有
    `stream.delta` → 没有 assistant 消息；而 `tool.start`/`tool.update` 都要求
    `m.back().role == "assistant"`，于是**工具块被整个丢弃**。同一个原因也解释了
    **计时器不显示**（计时器渲染在那条消息上）。新增 `ensure_turn_message()`，在
    `tool.start`/`tool.update`/`assistant.interim` 里按需创建该轮消息。
    另外工作过程的渲染门槛加上了 `interim`（只有旁白的轮次也要有折叠块）。
  - **高度**：工作过程与工具详情**都不再设上限**（10 行 / 28em / 78vh / 220px / 65vh 都试过，
    都让看长结果变成在小盒子里滚动），随内容增长、由页面滚动。
  - **子代理记录放回侧栏**：上一轮做成了全屏页面（`sa:<id>` 标签），用户指出两点问题——
    子代理就该在侧栏、且全屏页关不回会话。现在是一个 `kind="subagent"` 的**侧栏标签**
    （带 `subagent_id`、可多开、可关闭），卡片点击与工作过程「打开记录」都指向它；
    已结束的代理照样能打开。全屏页面与 `build_subagent_page` 已删除。
  - **测试**：新增 3 用例（两个并发子代理 + 持续 tick/list 不死锁；follow_up 不持锁能返回；
    记录剪枝且运行中永不剪）。372 用例 3589 断言全过；web / webview 编译通过。

<83>(SOLVED): 闪退丢记录（live 快照落盘）+ webview 关闭崩溃（dispatch 悬垂指针）+
  挂起等后端 + 发送变中止 + 模型带供应商 + 思考档位真接线 + 侧栏记录可滚动 + 去掉手动定时。
  - 用户反馈：① `Failed to unregister class Chrome_WidgetWin_0. Error = 1411`，子代理窗口
    似乎不能滚动；② 模型显示要带供应商，"自动唤醒"手动按钮不要，思考强度按钮没做出来；
    ③ 挂起不该点一下就切换（要等后端状态）；④ 发送后按钮应变"中止"；⑤ **闪退的聊天记录
    没记下来**，说明不是真实时（内存缓存没在异常时写盘），上一轮内容丢失。
  - **闪退丢记录（真 bug）**：一轮只在**结束时**才 append 到会话文件，所以进程一崩整轮就没了；
    之前的 live 快照只在内存里——崩溃正好把它一起带走。现在 `maybe_persist_live()` 在**流式期间
    每 ~2s** 把在飞回合写进 `runtime.json` 的 `live`（正文 + **userText** + startedAt），
    落地时写一次 `{running:false}` 清空（否则重开会把已入库的回答当崩溃恢复再显示一遍）。
    `session.liveTurn` 变两级：内存在跑→原样返回；没跑但盘上有 running 快照→
    `{crashed:true, running:false}`，UI 渲染成一条**已完成**消息并提示已恢复。
  - **webview 关闭崩溃（`Chrome_WidgetWin_0`）**：`webview_dispatch` 的任务捕获 `&ui`，
    而 `webview_run()` 返回后队列里可能还有未执行的任务 → 它们在 `webview_destroy()` 期间
    访问已析构的 `UiApp`（Chrome 注销窗口类正好撞上，所以报的是 unregister class）。
    改为 `PumpCtx{app, alive}` + `shared_ptr<atomic<bool>>`（按值进回调），关闭顺序
    `running=false → join → alive=false → app=nullptr`，回调先查 alive。
  - **侧栏记录不能滚动**：`.side-body` 是 flex 列，而 `.sap-msg` / `.subagent-card` 带
    `overflow:hidden` 且默认可收缩 → 内容被裁掉而不是溢出，面板没有可滚动的内容。
    加 `flex:none`（并给长 token `overflow-wrap:anywhere`）。
  - **思考档位"没做"**：合成器用 `thinking_levels_for(模型名)` —— 那是硬编码白名单
    （o1/claude/gemini/deepseek-r…），任何自定义/新模型都返回空 → 控件**根本不渲染**。
    改用 `model.allEnabled` 里该模型的 `thinking.levels`（模型表是权威），猜名只作兜底。
  - **挂起等后端**：点击只发请求，`refresh_loop_state` 回读 `session.loopState` 决定文案；
    请求在飞时按钮禁用（连点会发出 suspend+resume 两条相反请求）。
  - **发送 ↔ 中止同键**：新增 `turn_running`（来自 turn.started / turn.done /
    loop.interrupted / loop.paused / session.loopState），为真显示**中止**
    （走 `session.suspend`：当前工具返回后 park、保留转录、可继续）。
  - **模型按钮带供应商**：`主名 · 供应商`（dim、非等宽），数据来自 `providerName`。
  - **去掉手动"定时唤醒"**：排程是模型的动作（`session.schedule`）；用户唯一相关的操作是
    **取消**一个已排唤醒，只在真有待唤醒时出现在挂起旁边。
  - **测试**：新增 2 用例（流式期间有带正文+问题的快照、落地后最后一次写是清空；
    换一个 FsApi 进程仍能读回快照并标记 crashed）。374 用例 3583 断言全过；
    web / webview 编译通过。
  
 
<84>(SOLVED): 思考档位终于出现（无档位模型回退 disable/enable + 出网归一化）+ 输入框
  补全可用（Tab 真的能接受）+ markdown 提示条从不显示的真因 + ui_app.cpp 批量误删后的恢复。
  - **背景**：上一轮我跑批量文本替换时执行了 `.replace('tr("','"')`，把 `src/ui/ui_app.cpp`
    里约 680 处 `tr()` 调用前缀删掉，还顺带损伤了 `c_str(`→`c_s`、`attr(`→`.at`、`substr(`→`subs(`。
    之后每一版"修复脚本"都让文件更糟。最终收尾办法是 **token 级对齐**：把当前文件与 HEAD
    各做一遍词法切分、都去掉 `tr(` 后做 `difflib.SequenceMatcher`，只对"与 HEAD 唯一对应且
    差异恰好是删除 tr() 包裹"的行按 HEAD 还原（24 行），其余靠编译器逐条修。教训已写进
    this entry 的末尾。
  - **思考档位从来没有渲染（真因）**：用户的模型（`deepseek-flash` / `deepseek-v4-pro`）在
    配置里就是 `caps.thinking=false` + 空档位表，而 UI 的渲染条件是"档位表非空"。于是控件
    永远不出现，跟样式、宽度、CSS 都无关。现在：档位表为空且已选模型时，回退成最粗的两档
    **关 / 开**（`disable` / `enable`），默认项仍是"默认"（= 什么都不发），所以仅仅渲染这个
    控件不会把请求改坏；用户明确选了"开"才发提示。
  - **回退值不能直接出网**：没有哪个厂商接受字面上的 `enable`（OpenAI 认 minimal/low/medium/
    high，Anthropic 根本没有命名档位，其他家把未知 effort 当 400）。新增 `wire_thinking()`
    在**唯一触碰网络的那一层**归一化：`disable/off` → 整个字段省略；`enable` → `medium`；
    其他原样透传。Anthropic 分支同时补齐 minimal/max 的预算映射，OpenAI **Responses** API
    补上 `reasoning.effort`（此前完全没处理，"Responses 供应商上设思考档位毫无作用"）。
  - **输入框补全：Tab 真的能接受了**。`accept_complete()` 只改 `st_.session_input`，而框架
    **故意**不给获得焦点的 textarea 回写 value（否则每次敲键都跳光标）——所以接受的文本从未
    进 DOM，表现就是"按 Tab 菜单关了但一个字没插入"。现在照 `send_message` 清空输入框的做法，
    把新文本**直接写进节点**。同时两个浏览器后端在 `data-keys-capture` 存在时对 Tab/↑/↓/Esc
    调 `preventDefault`：不然浏览器先移焦点/滚页，按键根本轮不到 app；只有补全菜单开着时
    才带这个属性，别处的 Tab 导航与方向键滚动不受影响。
  - **markdown 提示条从不显示（真因）**：检测代码写在 `build_session()` 里，而它只在 `render()`
    期间跑；本 UI 又故意"打字不重绘"。结果检测函数位于唯一永远不会被调用的位置。现在拆出
    `maybe_update_md_hint()` 在**输入回调**里调用，且只在识别集合变化时才安排一次重绘
    （不是每次按键）。实测：输入 `**bold** 和 \`code\`` → 显示 `bold · inline code`，
    原文一字未改。
  - **设置页只剩两个阈值旋钮**：`自动压缩阈值`（60/70/80/90/关闭）与 `保留最近轮数`。
    "最近轮次"窗口和"token 硬上限"已删除——它们是**每轮都会触发**的隐形压缩，跟水位无关。
    新增用例把这两个幸存者钉住，防止以后被悄悄加回来。
  - **测试**：新增 6 用例（无档位回退、有档位优先、wire 归一化三态、Anthropic 预算、
    markdown 提示条打字即现且不改原文、设置页只剩两个旋钮）。380 用例 3611 断言全过；
    web / webview 双变体编译通过。
  - **浏览器实测（127.0.0.1 真实页面）**：思考控件出现且为 `默认/关/开`；输入区仍是**单行**
    28px 无溢出；供应商 `DeepSeek` 存在但在窄宽度下 `display:none`（紧凑模式不显示厂家）；
    开头 `/` 弹技能列表、↑/↓ 移动选中、**Tab 插入 `/new `**；句中 `./sr` 弹 `src/` 候选；
    markdown 提示条显示 `bold · inline code`。
  - **踩坑记录（写给下一次）**：**不要**对源码做"先删再补"式的批量正则替换。字面量
    `.replace('tr("','"')` 这类操作会删掉函数名前缀并留下不成对的括号，而后续每一版修补脚本
    都会在已经损坏的文本上继续猜，越修越坏。正确做法是 token 级比对 + 按行还原，或者
    `git checkout` 后重放编辑。无法用 Edit 工具时，用 `io.open(..., encoding='utf-8', newline='')`
    的 Python 脚本，绝不用 PowerShell 重写含中文的源文件。

<85>(SOLVED): 思考档位按模型配置（去掉伪"默认"）+ 空图标按钮 + 删掉 markdown 提示条 + 子代理记录落盘 + 滚动恢复延后。
  - **用户纠正了我上一轮的设计错误**："你不会以为用'默认'表示模型设置里的吧？应该是模型配置了
    选项这里放上，我说的 enable 那些是作为 fallback 的" —— 完全正确。现在下拉框只列模型表里
    的 `thinking.levels`，并预选模型自己的 `thinking.default`；`disable`/`enable` 仅在该模型
    没有任何档位时才作为 fallback 出现。伪"默认"选项已删除。
  - **"模型选择与思考强度中间的空按钮"真因**：`thinking_icon()` 对"默认/关/未知"返回
    `lightbulb`，而图标子集里**没有** `fa-lightbulb`（只有 `fa-brain`），于是渲染成一个空白
    按钮。改用存在的 `fa-brain`（关档用 `fa-bolt` 区分两端）。
  - **改完模型设置输入框不更新**：`save_model()` / `toggle_model_enabled()` 之后只重载设置页的
    模型列表，从不刷新合成器。两处都补 `refresh_model_availability()`。
  - **markdown 提示条删除**："我输入什么上面给我一个 bold 之类的字样…还不是实时的…甚至显示还会
    让输入框失去焦点"。textarea 无法局部上样式，而"打字不重绘"是硬约束（否则跳光标），所以那
    个提示条既不实时也无用。整套（`md_hint_for_inner` / `maybe_update_md_hint` / 状态字段 /
    `.composer-mdhint` 渲染）已移除。
  - **滚动恢复真因（第二轮）**：之前把 `set_scroll_top` 放在 `render()` 之后**同步**执行。
    render 只是 patch 了 VNode，浏览器还没对新内容做布局——此时赋 `scrollTop` 会被**旧高度**
    钳位（500 → 0），随后该次 scroll 事件又把 0 写回保存值。现在抽出
    `schedule_history_scroll_restore()`，在下一个 tick 执行，并再延一 tick 才松开
    `restoring_scroll_` 守卫。实测：滚到 500 → 切到别的会话 → 切回来，仍是 500。
  - **子代理记录打不开（用户猜对了）**：记录只在内存里，重启即丢；而且 `turn_to_message()`
    重建历史时**没有**恢复 `subagent_id`，所以刷新后"打开记录"按钮本身就消失了。三处修复：
    ① 循环把 `subagentId` 作为正式字段写进 toolCall；② UI 重建历史时优先读该字段、回落到
    解析 detail JSON；③ 记录完成时落盘到 `<session>/subagents/<id>.json`，
    `subagent.detail` 新增 `subagent_detail_in(sid, id)` 在内存 miss 时读盘。
  - **顺手清掉死字段**：`HistoryBuilder::Options::recent_turns` / `token_budget` 已无任何
    读取点（压缩只剩水位 + 强制），连同测试里的赋值一并删除，避免以后再被误当成"有效旋钮"。
  - **测试**：新增/改写 6 用例（档位 fallback、档位用模型配置并预选默认、会话选择优先、
    滚动恢复延后、子代理 id 跨重载存活、记录跨进程存活、id 清洗）。384 用例 3635 断言全过。
  - **构建踩坑**：emcc 的 acorn 压缩步骤偶发崩溃（返回码 3221225477 = 0xC0000005 访问违例），
    重试即可；`-Os` 触发。另注意 wasm 目标是 per-variant 的：切到 webview 后 `fs_ui_wasm`
    不再是有效目标，要 `xmake f --variant=web` 才会回来。

<86>(SOLVED): 思考档位只剩一个控件（每档一个图标 + 未知用灯泡）+ 挂起/恢复真的能继续 + 模型选择器打开在对应页 + 页面切换滚动改同帧恢复。
  - **"思考档位，思考档位（重复！这个按钮按下没反应）"**：合成器里其实是**两个**东西——
    一个 `icon_text_button(..., nullptr, ...)`（没有 handler，按了当然没反应）紧挨着一个
    原生 `<select>`，两者显示同一个值。现在只剩**一个**图标按钮：点开是档位列表，每行带
    自己的图标和勾选。窄容器时 `.it-label` 自动收起 → 只剩图标（容器查询，不是新机制）。
  - **"图标也只有关闭和其他"**：`thinking_icon()` 对除 off 以外的档位一律返回 `brain`。
    现在每一档一个专属字形：关=power-off、最低=feather、低=gauge、中=brain、高=fire、
    超高=gauge-high、最大=infinity、自动=wand-magic-sparkles，**未知档位统一灯泡**
    （"未知的选项就用个通用的灯泡或大脑吧"）。顺手发现图标子集里 `fa-brain` 的码位是
    `f19c`（错的字形），重新生成后是 `f5dc`；`fa-lightbulb`/`fa-fire`/`fa-gauge*`/
    `fa-power-off`/`fa-feather`/`fa-infinity` 一并补进 `tools/make_fa_subset.py` 的
    WANTED + FALLBACK，重新生成 `web/icons/fa-subset.css`（旧图标零丢失，已用脚本对比）。
  - **挂起"功能疑似没做全"**（三处真缺陷）：① 循环真正停车后 `suspend_requested_` 已被清掉，
    `loop_state()` 于是报 "idle"，UI 立刻退回"挂起"按钮——看起来像任务结束；新增
    `AgentLoop::parked_`，停车时置位，`loop_state()` 也认它。② `resume()` 只清标志，停车后
    没有任何线程会再看它，**恢复是空操作**；新增 `resume_task()` 重跑 `saved_messages_`，
    `AgentHost::resume` 在**锁外**调用（同 `send()` 的约束）。③ 空闲会话上常驻一个
    永远无效的 挂起 按钮；现在只有 `turn_running || suspended` 才渲染。另外挂起中发消息
    走 `continue_task(text)`（带引导继续），不再开新轮把停住时的工具结果丢掉。
  - **模型选择器"不会自动放到选择的那一页"**：左列是供应商列表，打开时用
    `provider_selected`/第一个供应商，当前模型常常在别的页。`open_model_picker` 现在记下
    `model_picker_focus`（会话模型 / 设置项当前值），`allEnabled` 回来后再把左列切到该模型
    的供应商。
  - **页面切换滚动（第三轮，这次改机制）**：前两版都错在同一点——**在后面的 tick 里设
    scrollTop**：内联版本按旧高度钳位成 0 并被 scroll 事件写回；延后一拍版本先以 0 画出
    来再跳过去（"这次是到顶上了"）。现在 `request_history_scroll_restore()` 只记目标，
    **同一个 `render()` 里** patch 之后先读 `scroll_height()/client_height()` 强制布局、
    再 `set_scroll_top()`——第一次绘制就是对的。`switch_tab` 也走这条路径（缓存视图
    已有消息，不必等网络）。恢复期间 `restoring_scroll_` 抬起两拍，滚动回调不写回。
  - **测试**：新增/改写 8 个用例（单一思考控件 + 菜单打开/点击选中、每档独立图标、未知档位
    灯泡、挂起后 `resume_task` 真的继续、挂起中发消息续跑而不是开新轮、滚动恢复**同一 render**
    生效）。388 用例 3642 断言全过；web / webview 双变体编译通过。
  - **验证方式的坑**：这次内置浏览器面板起不来（`browser guest not attached (webview not
    ready)`，两次都一样），于是改用 **HTTP 断言**验证产物：服务端 `/style.css` 里
    `.think-menu` 存在、`.thinking-group` 已删除；`/fs_ui.wasm` 里 `think-menu` 出现 4 次、
    `thinking-sel` 0 次。UI 结构本身由 NullDom 用例逐条断言（比肉眼更可靠）。

<87>(SOLVED): 编辑并重发（按钮此前根本不渲染）+ 停止/挂起后消息定格 + webview 滚动三处真因（切页回顶部 / 到顶不加载更早 / 终端不跟随）+ 删除假的浏览器控制 skill。
  - **"4 并没有修好，我试了还是原来那样" 的真因**：`chat_message()` 的 user 分支在拼完
    `bubble + meta` 后直接 `return`，而 `编辑` 按钮只加在 assistant 分支的动作行里——
    按钮**从来没有渲染出来过**（用 CDP 查 `.msg.user .msg-actions` 得空数组）。
    现在 user 气泡 hover 出 `编辑`，且只给**最后一条** user 消息（改更早的问题会让之后的
    回答全悬空，那是 回滚 的活）。
  - **编辑改成"先 arm、发送时才截断"**：点击只把文本放回输入框并记 `edit_turn_id`，转录一行
    不动，合成器上方出现"正在编辑上一条消息 / 取消编辑"条；真正发送时先
    `session.trimHistory`，回调里把本地转录从那一轮截掉，再走原来的 `dispatch_message()`。
    这样点了编辑又改主意、或 trim 失败，都不会丢历史、也不会留下重复的问题。
  - **停止生成/挂起确实没做全**：park 不发 `turn.done`，最后一条 assistant 消息的
    `streaming` 永远为真（转圈 + 计时器继续走）。`loop.state = suspended` 现在自己定格
    （`streaming=false`、补 `ended_at`），一个字都没产出的空消息直接丢弃。顺手删掉一个
    永远走不到的重复 `loop.paused` 分支。
  - **webview 切页回到顶部（真因，三处都在 glue 里）**：
    ① `bind_scroll_edges`/`bind_scroll_pin` 传的是空字符串当"回调 id"，JS 里
    `parseInt("")||0` = 0，监听器开头 `if (!el.__fsEdgeCb) return` 直接退出；
    ② 滚动镜像 `push('scroll', el.__fsEdgeCb, …)` 用回调 id，而宿主 `on_push` 按**节点 id**
    写槽位，于是宿主眼里的滚动位置恒为 0 → 切走保存 0、切回恢复 0；
    ③ 连带 `at_top` 从不触发，回到顶部**不会加载更早的轮次**（终端 at-bottom 也一样死）。
    修法：绑定传 `"1"` 作启用标志、镜像用节点索引 `i`；`at_top` 与 wasm 对齐成 24px。
  - **本地第一次真正验证到原生窗口**：给 WebView2 加
    `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9229`，直接用 CDP 驱动
    `flashsight-webview.exe` 的页面。结果：长会话滚到 500 → 主页 → 切回仍是 500（修前 0）；
    顶栏标签互切（长会话 700 ↔ 第二个会话 3140）也各自保持；
    滚到顶 30 → 47 条（加载更早一页）；思考菜单在按钮上方 6px、右边缘对齐；
    最后一条 user 消息有 `编辑`、更早的没有；气泡圆角 `10px 2px 10px 10px`。
  - **假完成继续清**：删除内置 skill `browser-control` 和 `browser.load` 路由——它描述
    navigate/query/act/eval，却没有注册任何对应工具，还指向已删除的"设置-浏览器控制"页；
    留着只会让模型照文档编造结果。验证浏览器改走 Playwright/本地引擎 + terminal。
    压缩页只剩 `自动压缩阈值`（`history_keep` 连状态字段一起删了）；`取消唤醒` 用的
    `fa-bell-slash` 之前不在图标子集里（空白图标），已补进 `fa-subset.css` 与生成脚本。
  - **测试**：391 用例 3670 断言全过；web / webview 双变体编译通过。新增
    `uiapp_last_user_message_is_editable`（按钮只在最后一条 + 点击只 arm + 发送才 trim/send）、
    `uiapp_suspend_finalizes_the_live_message`（park 定格、空消息丢弃）；skill 双语用例改用 lsp。

<88>(SOLVED): 会话完整性（失败/异常也落盘 + 工具输入显示 + 发送即现）+ 读文件编码（UTF-8/UTF-16/GBK 自动识别）+ 长文本横向悬浮滚动条 + 打开会话不再残留"中止"。
  - **用户实测的 s_fbaea8c3f7f2（C:/aaa/Sliom）**：磁盘上确实只有 user 轮，assistant 轮
    一个都没有——"重新打开发现真没了"是真的。真因链条：`read_file` 把 GBK 原文当字符串
    返回 → 进 tool result → `nlohmann::json::dump()` 在非法 UTF-8 上抛 type_error.316 →
    异常一路穿出 `run_loop`，**落盘代码根本没执行**。
  - **读文件编码**：新增 `utf8::decode_to_utf8()`（BOM → UTF-8 → 系统 ANSI/GBK →
    GB18030 启发 → Latin-1 兜底，永远返回合法 UTF-8），`read_file`/`find_files` grep/
    侧栏预览/AGENTS.md（系统提示词！）全部先解码；GBK 文件的结果里带 `"encoding":"gbk"`。
    UTF-16 无 BOM 的判定改成"每个 2 字节对有一侧是 NUL"的奇偶性启发（旧的宽松判定把
    `ab\0cd` 这种 5 字节文件当成 UTF-16，二进制检查因此失效）。
  - **序列化永不抛**：`dump_safe()`（`error_handler_t::replace`）用在工具结果、provider
    请求体、历史投影、落盘（`write_atomic`）。`run_loop` 外面包了 try/catch → `fail_turn()`：
    已流出的文本 + 已跑完的工具调用 + 原因写成正式 assistant 轮（`failed:true`/`error`），
    再发 `loop.interrupted`；provider 失败的两个分支也调 `persist_failed_turn()`。
    实测（本地 mock provider，read_file 读 GBK 文件 + 第二轮返回 400）：磁盘上出现了
    `{"error":"http 400: mock provider failure","failed":true,...}` 的一轮。
  - **"history 还显示加载中"**：`loop.interrupted` 同样没有 `turn.done`，UI 现在自己定格
    （`streaming=false`），并把原因放进消息（`.turn-error` 行）。首轮就失败、还没有
    assistant 消息时补一条只有错误的消息——否则问题后面空无一物，像发送被吞了。
  - **"发送以后 history 没有反应，切页面才显示"**：`session.turns` 的回复描述的是**请求
    那一刻的磁盘**，比屏幕旧。整表覆盖会抹掉乐观追加的用户消息和正在流的内容。现在合并：
    保留本地 `turn_id` 为空且文本不在回复里的消息，追加在回复之后。
  - **打开会话还显示"中止"**：`turn_running` 来自切走时暂存的视图，打开会话时没有任何
    地方和后端对一次。`load_session` 现在调 `refresh_loop_state`；同时那条查询加了序号，
    **只认最新一次请求的回复**（否则回合结束后晚到的 "running" 又把按钮改回中止）。
  - **"read_file 只有返回 json 没有输入"**：`tool.start` 的 args 原本写进 `detail`，而
    `tool.update` 会用结果覆盖它。现在 `ToolBlock::input` 是独立字段，展开后是"输入 /
    返回"两段（只有一段时不加标签），历史和实时都恢复；`turn_to_message` 从 `tc[k].args`
    读回来。mock 实测：输入 `{"path":"src/gbk.txt"}`、返回带 `encoding: gbk`。
  - **"长文本 x 轴无法滚动看不全"**：`.tb-body` 是 `overflow: visible` + `pre`，长行被
    `.tool-block { overflow: hidden }` 直接裁掉。现在 `white-space: pre; overflow-x: auto`
    + **悬浮横向滚动条**（透明轨道，hover 才显示拇指），实测 600 字符行 scrollWidth
    4239 > clientWidth 950 且可滚；`.tb-label` 是"输入/返回"的分段标签。
  - **测试**：395 用例 3709 断言全过；web / webview 双变体编译通过。新增
    `utf8_decodes_legacy_encodings`、`throwing_tool_still_persists_the_turn`、
    `uiapp_interrupted_finalizes_and_explains`（含"首轮失败补消息"）、
    `uiapp_turns_reply_keeps_live_messages`、`read_file` 的 GBK 用例。
  - **验证方式**：写了一个本地 OpenAI 兼容 mock（`temp/mock_openai.cjs`：第一轮发
    read_file 工具调用，第二轮给答案；用户文本含 fail 时第二轮返回 400），配合
    `--root temp/mock/data` 跑真实循环，用 CDP 驱动页面断言"发送即出现""输入/返回两段"
    "中断行""重开后仍在"。沙箱 sid 必须是 `s_`+十六进制（`s_mockgbk…` 被 valid_sid 拒绝，
    表现为整页空白 + 选择模型）。

<89>(SOLVED): 横向滚动收拾干净（history 不再横滚 / 表格自己滚 / 工具体限高才够得着）+ 空输入段隐藏 + 状态圆点实时联动（项目·会话·标签·子代理）+ 健壮性补丁。
  - **"history 不应该有横向滚动"**：`.history` 从 `overflow: auto` 改成 `overflow-x: hidden;
    overflow-y: auto`。表格不再是"把整页撑宽"的元凶——markdown 表格现在包一层
    `.md-table-wrap{overflow-x:auto}`，表格本身 `width: max-content; min-width: 100%`，
    宽表自己滚、窄表仍然占满列。顺带：既然历史不再横滚，段落里的超长不可断词（URL/哈希/
    base64）必须 `overflow-wrap: anywhere`，否则会被直接裁掉。
  - **"横向滚动条并没有悬浮，记录过长会被挤到下面"**：`max-height: none` 是我上一版
    的错误决定——条确实存在，但在几千像素的下面。工具体现在是**有界窗口**
    （`max-height: min(40vh, 26em)` + `overflow: auto` + `overscroll-behavior: contain`），
    两轴的滚动条永远贴在可见窗口边缘，拇指 hover 才显现。实测：clientHeight 302 /
    scrollHeight 2247，两轴都能滚且框底在视口内。
  - **"工具调用输入为空没必要显示（{} 可以跳过）"**：组件层把只有括号/空白的输入视为
    没有输入（不再出现一个只有 `{}` 的"输入"段，也不再单独显示"返回"标签）；两个填充点
    （live 的 tool.start、历史的 turn_to_message）也跳过空对象。
  - **状态圆点的链条断裂（用户："项目那里的小圆点没有反应"）**：
    ① session 行状态（running / loopState / terminalCount / autoRound）只来自
       `session.list`，而它**只在加载时取一次** → 新增 `mark_sessions_dirty()`：
       turn.started/turn.done/loop.state/loop.interrupted/loop.paused/auto.next/tool.start/
       terminal.exit 以及"终端刚出现/刚 spawn"时标记，`pump()` 里 800ms 合并刷新。
       注意 terminal **output** 不标记（一直刷屏的终端会让列表每秒请求一次）。
    ② 当前会话那一行用 UI 自己的实时状态覆盖（`live_session_row`：turn_running /
       auto_round / 活动终端数 / interrupted 原因），点下去立刻变色，不用等网络。
    ③ 后台标签的 "dot"（SessionTab::dirty）**从来没有被写过**：后台 turn/流式事件现在
       会点亮它，切到该标签时清除。
    ④ `subagent.started` 后端一直发，UI 从来没处理 —— 面板打开后才启动的子代理没有行，
       它的流式文本按 id 找不到宿主（被丢弃），标签徽标也不涨。现在补行。
  - **健壮性**：`fail_turn()` 里的落盘与快照都包了 try/catch（它在 catch 块里运行，
    再次抛异常就会飞出处理器，连"已中断"的通知都发不出去）；测试覆盖异常落盘。
  - **验证**：CDP 实测 history scrollWidth == clientWidth（938）且 overflow-x hidden；
    宽表 wrap 1134 > 870 单独滚动；nav 圆点 running→done 与 中止/发送 按钮同步（web 与
    原生 webview 都验过）。
  - **测试**：399 用例 3724 断言全过；web / webview 双变体编译通过。新增
    `uiapp_nav_dot_follows_live_state`、`uiapp_background_tab_gets_dirty_dot`、
    `uiapp_empty_tool_input_is_hidden`、`comp_wide_table_gets_its_own_scroller`。

<90>(SOLVED): 操作记录实时刷新 + 蓝点看过后清除（切页也清）+ 侧栏默认打开用量 + 余额/额度按 endpoint 自动查询（每轮结束/进页面/切供应商）+ 供应商预设扩充与自动选中。
  - **操作记录（systemOps）是活的日志，却只在打开会话时取一次**：面板开着改权限/模型，
    列表一动不动。`pump()` 现在在 ops 面板可见时每 2 秒调 `refresh_session_ops()`
    （`session.info` 回来的列表相同就不重绘）。顺带发现"工具调用"面板读的
    `session_calls` **从来没有任何代码填过**——它永远显示"暂无记录"。现在直接从转录里的
    工具块生成（名字+摘要+✓/✗），并把那个死字段整个删掉。
  - **"工作完成会亮蓝点，可查看了不会消失，即使切页面也是"**：`loopState` 在后端一旦变成
    "turnDone" 就永远不变，而圆点把它当成"已完成"永久显示。现在圆点表示"你上次查看之后
    有新动静"：`session_seen_ms_` 在打开会话（以及你正看着的那一轮结束/中断）时记时间，
    行上 `updatedAt <= seen` 的 done/failed 状态不再显示。
    **踩坑**：`now_ms()` 是单调时钟（steady_clock / performance.now），和行里的 epoch
    `updatedAt` 比大小永远不成立——新增 `wall_ms()`（system_clock）专用于这种比较，
    否则"看过了也不消失"照旧。
  - **"侧边栏默认什么都不开却显示用量统计"**：两处原因。① `active_side` 从来没设过默认值，
    面板的 kind 兜底成 "usage"，所以内容在、标签却没一个高亮——`AppState::make()` 和
    `default_session_view()` 都把第一个标签设为激活。② `views_.get_or_create(sid)` 给后台
    事件造的是**裸 SessionView**（没有 side_tabs），切回该会话恢复出来就是"零个面板"——
    `load_session_view` 现在补回默认面板。
  - **余额 / 额度**：新增 `provider.balances`。没有统一标准，所以按 **endpoint** 识别供应商
    （改名成 "我的 API" 也还能认出 api.deepseek.com）：
    DeepSeek `/user/balance`、OpenRouter `/api/v1/credits`（credits−usage）、
    Moonshot `/v1/users/me/balance`、SiliconFlow `/v1/user/info`、
    OpenAI/网关 `/v1/dashboard/billing/credit_grants`。没有公开接口的（Groq/Mistral/xAI/
    通义/智谱/火山…）如实写"不支持 · 该供应商没有公开的余额接口"；请求失败写
    "获取失败：HTTP 401"之类，**绝不补 0**。解析逻辑独立在 `provider/balance_probe.cpp`，
    用 FakeNet 单测（DeepSeek/OpenRouter/Moonshot 的字段、401、坏 JSON、不支持、无 Key）。
    刷新时机：用量页进入（30s 节流）、每轮 `turn.done`（8s 下限，避免连发把供应商打限流）、
    切换用量范围/模型（换供应商）。UI 在用量页顶部加"余额 / 额度"一栏，一行一家，
    带 endpoint 和禁用标记。
  - **供应商预设扩充**：Groq / Mistral / xAI / Together / Fireworks / DeepInfra / Cerebras /
    MiniMax / 阶跃星辰 / 火山方舟（余额识别靠的就是这里填对的 endpoint）。
  - **供应商页**：默认选中第一个（原来就有）；**一个都没有时自动打开新增向导**（一次性，
    关掉后不会再抢）。
  - **测试**：405 用例 3758 断言全过；web / webview 双变体编译通过。新增
    `provider_balance_by_endpoint`（FakeNet）、`uiapp_done_dot_clears_after_viewing`、
    `uiapp_side_panel_defaults_to_usage`、`uiapp_ops_panel_refreshes_while_open`、
    `uiapp_calls_panel_reads_the_transcript`、`uiapp_providers_page_auto_selects`。
  - **实测**：用量页两行——Mock 显示"不支持"，一个指向 `api.deepseek.com.invalid` 的测试
    供应商显示"获取失败：HTTP 401"；侧栏默认 `ptab active`（用量）；一轮结束后圆点
    running→done→（看过后）idle，web 与原生都验过。

<91>(SOLVED): 余额进侧栏（用量页的余额挪到最后）+ 供应商图标（按 endpoint 认品牌，未知用首字母）+ 工作流改成"模型规划 → 用户允许 → 侧栏看进度" + 终端记录落盘（重启后还在）。
  - **余额进侧栏**：用量面板底部新增"余额 / 额度"一栏，一行一家（名称 + 金额 / 不支持 / 获取失败的
    原因，悬停看 endpoint）。打开面板会强制刷新一次（仍有 8s 下限）。用量页里的同一块移到了**最后**
    （先看用了多少，再看还剩多少）。
  - **"新增的供应商没图片"**：供应商列表的每一行原来**根本没有图标元素**（模板网格有，列表没有）。
    现在每行有一个 22px 的品牌标记：`provider_brand_for_endpoint()` 按 **endpoint**（再按名字）
    认品牌——deepseek / moonshot / openrouter / siliconflow / zhipu / dashscope / openai /
    anthropic / gemini / groq / mistral / x.ai / together / fireworks / cerebras / minimax /
    stepfun / volces / :11434 / :1234 / opencode——认不出的用首字母（DeepInfra 就没有 lobe
    图标，显示 "D"，不再是空白方块）。模板网格也补齐了新预设品牌（groq/mistral/xai/together/
    fireworks/cerebras/minimax/stepfun/bytedance）。
  - **工作流：模型规划、用户允许、侧栏看进度**。以前侧栏是"流水线列表 + 启动按钮"（用户发起），
    不符合"模型自己规划"的要求。现在：
    ① 工具 `workflow_plan {goal, steps:[{title, task?, kind?}]}`（取代 `workflow_start`）只
       **提交**计划：后端 `workflow_propose()` 落成 `state="proposed"` 的运行实例，每步交给一个
       complete-step 子代理，然后停下等用户；
    ② 侧栏面板不再是启动器：顶部是"待允许"的卡片（目标 + 编号步骤 + **允许 / 拒绝**），
       下面是运行中/历史的步骤进度（✓/●/○、失败步可重试/跳过、运行步可引导）；
    ③ `workflow.approve`（唯一真正开始执行的地方）/ `workflow.reject`，以及 `workflow_proposed`
       事件 → pump 里节流刷新面板，计划出现不用重开面板。
    实测：API 提交 2 步计划 → 面板显示"模型规划了 2 步，允许后开始执行" + 步骤 → 点允许 →
    "运行中 ✓ 调查现有实现 ● 写测试"。
  - **"打开过的终端没保留，文件夹里也是空的"**：`terminals/` 目录只在建会话时 mkdir，**没有任何
    代码往里写**（`terminal_log_path` / `list_terminal_logs` 是死代码）。现在：
    ① 每次 spawn 把元数据写 `<session>/terminals/<tid>.json`，把 PTY 输出追加到 `<tid>.log`
       （超过 4MB 自动保留最后 1MB，从行首开始）；
    ② 退出时把 status/exitCode 冻结进 json；
    ③ `terminal_list` 先把磁盘记录**装回注册表**（`TerminalRegistry::add_saved`，强制 exited、
       不覆盖活动终端、并抬高 tid 序号避免复用日志文件名），所以重启后列表还在；
    ④ `terminal_grid` 对没有进程的终端**从日志重建**（去掉 ANSI 序列、CRLF 归一、单独的 \r
       按重绘取最后一段，保留最后 2000 行）。
    实测：API spawn 一个 cmd → `terminals/1.json` + `1.log` 落盘 → 杀掉进程重启 → `terminal.list`
    仍返回 tid=1（status=exited、logAttached）→ `terminal.grid` 回放出原来的提示符与命令。
  - **测试**：408 用例 3829 断言全过；web / webview 双变体编译通过。新增
    `addon2_workflow_propose_needs_approval`（提交→待允许→拒绝→再提交→允许）、
    `uiapp_workflow_panel_shows_the_plan_for_approval`（待允许卡片、步骤、允许按钮）、
    `terminal_record_survives_restart`（磁盘记录 → 列表 → 日志回放，ANSI 剥离）。
  - **顺手修的链条**：`ops` 面板在重写工作流面板时被误删，已补回并按时间倒序显示；
    工作流类事件现在也会让会话行刷新（运行中的工作流算后台工作）。

<92>(SOLVED): 终端历史改成轮转（不丢）+ 输入单独记 + 搜索进历史 + 输出按 sink 实时落盘（重建不再缺尾）+ 余额查询并行/5s + 顶栏标签细滚动条。
  - **"过多的历史压缩处理…不主动丢弃"**：上一版把超过 4MB 的日志截成最后 1MB——那是丢弃，不是
    压缩。现在 `1.log` 满 4MB 就**轮转**成 `1.<epoch>.log`，链条一直留着；回放读整条链，搜索
    也扫所有轮转档。
  - **"搜索可能输入还要单独保存"**：用户敲进去的内容单独写 `<tid>.input.log`（程序不一定回显：
    密码提示、全屏程序），新增 `terminal.search {tid,q}` 同时扫输出链 + 输入日志，返回
    `{file,line,text}`。侧栏筛选框对"恢复出来的终端"也能用：回放时把文本折进 `term_outputs`。
  - **"输入命令运行后直接退出，重建不完整"（真因）**：日志原来写在 `terminal_tick()` 里，tick 是
    60ms 一次的轮询——命令跑完立刻关应用，最后一段输出就永远没写进去。现在由
    `TerminalManager::feed_screen` 的 **output sink** 在字节落地的同一刻写盘（sink 是实例成员，
    不是 static：静态版会活过它的 FsApi，单测直接构造 TerminalManager 时会调到已析构对象，
    表现为整个测试挂住）。
    **顺带去掉假报错**：`terminal_grid` 对没有任何记录的 tid 现在返回**空网格**而不是
    "not found"——刚恢复的终端允许是空的。另外 `tick_terminal` 以前只给 running 的终端拉网格，
    恢复出来的终端永远显示"暂无输出"：现在每条记录**拉一次**回放（`term_replay_done_`），
    标签页和"所有终端"卡片都不用先点一下才出内容。
  - **"打开时获取余额导致界面迟迟不更新"**：`provider_balances` 现在**每个供应商一个线程**（每家
    5s 超时），总耗时 ≈ 最慢的一个而不是求和；之前 2 家 × 8s 串行，阻塞感就是这么来的。UI 侧
    本来就是回调式异步（browser= fetch，桌面= worker 池），面板先画"查询中…"再落数据。
  - **顶栏标签太多放不全**：`.tabs-scroll` 原来把滚动条藏了（`scrollbar-width:none`），开十几个
    会话根本看不出还有标签。现在 4px 高、hover 才显拇指的细条（VS Code 做法），只占 2px 高度。
  - **测试**：408 用例 3837 断言全过；web / webview 双变体编译通过。`terminal_record_survives_restart`
    扩展成覆盖轮转档 + 输入日志 + `terminal_search`。实测：起 cmd → 敲 `echo alpha-live` + `dir`
    → **立刻杀进程** → 重启：`terminal.list` 有记录、`terminal.grid` 12 行完整（含中文与两个命令
    的输出）、`terminal.search "alpha-live"` 命中输出 2 处 + 输入日志 1 处。

<93>(SOLVED): cwd 真因（模型的终端工具绕过项目根）+ 工作过程合成一条流并默认展开 + webview 分隔条方向 + 每次 API 调用实时更新用量 + 新项目自动建会话 + 顶栏滚轮/中键。
  - **`git log` 读到的是 FlashSight 自己的提交（严重）**：模型工具走的是
    `deps.terminal_spawn -> TerminalManager::spawn`，**绕过了 `FsApi::terminal_spawn`**，
    而项目根 cwd 的缺省逻辑只写在后者里。于是命令在**应用进程的目录**（release 目录，
    正好在本仓库内）里跑。现在只有一个入口 `FsApi::spawn_terminal(args)`：缺省 cwd =
    会话的项目根（pid → path → meta.cwd 逐级回落）、缺省编码 = 设置里的编码，并且给
    **每个**终端（包括模型起的）分配输出日志。实测：工具调用 `git log --oneline -3` 在
    mock 项目里返回 `57a6a87 mockproj first commit`（修前是 FlashSight 的提交）。
  - **"工作过程的输出保留在外层，重新进入不再显示"**：工作过程折默认是**关**的，重开
    会话时整段过程都看不见；而且里面的"思考 / 过程说明 / 工具调用"被分成三段。现在：
    ① 工作过程**默认展开**（想收起来点头部）；② 三段合成**一条流**——思考在最上，
    过程说明和工具块按顺序交替，不再有"工具调用"这个小标题；③ 工具体**不再限高**
    （之前为了够得着横向滚动条加了 max-height，反而变成里外两层滚动），横向仍然可滚。
  - **"侧边栏拖动方向和实际方向又相反了"**：webview 的 `splitter()` 三个绑定符号全反了
    （`split` +1、`vsplit` +1、`navsplit` -1），而 wasm 适配器是对的（右侧面板分隔条在它
    **左边**，所以拖动向左 = 变宽）。统一成：split -1 / vsplit -1 / navsplit +1。
    实测：右侧面板向左拖 60px，宽度 420 → 480。
  - **上下文/缓存要按每次 API 调用更新**：循环在每次模型返回后发 `loop.usage
    {sid,in,out,cached}`，侧栏据此立刻更新上下文水位、缓存命中、本轮/累计 tokens
    （此前只有 turn.done 之后的一次 usage.session 往返）。实测：回合进行中侧栏已显示
    `上下文水位 1% (100 / 128.0K)`、`本轮 200/40`。
  - **新项目只出现在主页、不在侧边栏**：侧栏按"项目 → 会话"列出，没有会话的项目根本
    不存在。`project.open`/`project.create` 现在会给**没有任何会话**的新项目自动建一个
    空会话（已有会话的项目不动）。实测：打开 freshproj → `session.list` 返回 1 条。
  - **顶栏**：滚轮在标签条上**横向滚动**（纵向滚轮转成 scrollLeft，横向滚轮/触控板保持
    原生），中键**不再触发 Chromium 的自动滚动**（mousedown/auxclick 都 preventDefault），
    同时标签条滚动条是 4px 细条（hover 才显拇指）。
  - **测试**：411 用例 3863 断言全过；web / webview 双变体编译通过。新增
    `terminal_spawn_defaults_to_the_project_root`（缺省 cwd 必须是会话项目根）、
    `project_open_creates_a_first_session`（且重复打开不重复建）、
    `uiapp_per_call_usage_updates_the_panel`；工作过程的两个折叠用例改成"默认展开、
    收起后隐藏"，并断言不再出现"工具调用"分段标题。

<94>(SOLVED): 工作过程"跑着展开、跑完折叠" + 过程解说不再混进答案 + 工作过程滚轮穿透 + 代码预览不换行且行号对齐 + markdown 渲染/源码一键切换 + 图片/二进制预览 + 工具卡死防护（整文件读取无上限、递归 grep 扫生成目录）+ 目录列表上限。
  - **疑似 `edit_file` 卡死程序和系统**：没有找到 `edit_file` 自身的死循环（行替换是线性的、edit 数也有限），
    但找到一类**真的能把机器拖进 swap、连桌面一起卡住**的无界工作，而 `edit_file` 正好是最容易被指到的入口：
    ① `edit_file` / `write_file` / `remove_file` / `regex_replace` 都走 `read_text_file` **整文件读入**，
    **没有任何大小检查**（`read_file` 有 32MB 硬上限，这几个没有）。模型一次路径写错指到构建产物
    （几百 MB 的 exe / 上 GB 的 pdb），内存瞬间翻倍 → 系统换页 → 程序和桌面一起冻。
    ② `find_files` 的 grep 是**整项目递归**，只跳过顶层的 `.git` / `.flashsight`：`build/`、`.xmake/`、
    `.zcode/`、`node_modules/` 全被读进来并逐个解码。whaleui 的 temp 几百个文件 + build 上千个产物，
    一个 grep 就是几百 MB 的读 + 解码，CPU 打满、UI 事件被饿死——这同时解释了"temp 文件夹几百个文件，
    扫描极其卡顿"。
    现在：`read_text_file` 返回 `ReadStatus{kOk,kMissing,kTooLarge}`，默认 32MB 上限；`edit_file` 超限
    明确报"文件太大，改用终端或更小的文件"（不再谎报 not found）；write/remove/regex 同样有界。
    `find_files` 按目录名跳过生成目录（任意深度），单文件 4MB、整次扫描 64MB 预算，结果用
    `truncated` + `note` 如实说明"跳过了 build/.git/..."。`project_files` 单目录最多 2000 项 +
    `truncated`，侧栏显示"目录太大，只列出前 2000 项"。
  - **"正在跑的应该展开工作过程，跑完默认自动折叠"**：上一批把默认改成了"一直展开"，方向过头了。现在
    `ChatMsg::work_open` 默认 false（历史/重开的回合是折叠的）；本轮**新建消息时置 true**，`turn.done`
    和 `loop.interrupted` 折叠；渲染层"运行中 = 展开"（`streaming && !work_touched`），用户自己点过折叠
    后按用户的来，直到本轮结束。折叠头仍然给摘要："工作过程 · 1 步 · 1 段过程说明 完成 5.8s 13:26 ▸"。
  - **"过程输出的文字还显示在总结里，重开/切页就没了"**：同一轮解说先以 `stream.delta` 进了答案，
    轮末又作为 `assistant.interim` 到达；UI 只把它 push 进工作日志，**没有从答案里拿掉**。于是实时记录里
    它出现两遍（还当成了总结），而存储的 turn 只把解说放在 `interim` 里——重载/切页重建后自然"消失"。
    现在 `assistant.interim` 到达时把这段文字从答案尾部移除（ends-with，退化为最后一次出现），
    实时和重载看到的是同一份内容。
  - **"工作过程的小页面鼠标滚动不动，应该整页动"**：`.tb-body` 上的 `overscroll-behavior: contain`，
    而它纵向根本没有可滚内容——滚轮就"消失"在这个块里，页面不动。去掉；`.work-body .think-body` 也
    不再限高（原来 10 行小窗），整个工作过程随页面滚。实测（CDP 真实滚轮事件）：指针停在工具结果上
    滚 320px，`.history` 0 → 759（web）/ 0 → 3501（webview），`tb-body.scrollTop` 保持 0。
  - **"渲染代码有自动换行，但行号没跳"**：`.file-preview-body` 是 `pre-wrap`，行号列是 `pre`——
    长行折行后下面的行号全部错位。现在正文 **`white-space: pre`**（不折行，横向滚动），
    `min-width: max-content` 撑到最长行；行号列 **`position: sticky; left: 0`** 贴住左边。
    实测：2908px 的行宽在 399px 的面板里横向可滚，行号 1..5 始终对齐。
  - **markdown 一键切换渲染/源码**：侧栏预览里 `.md` 默认**渲染**（复用聊天的 `markdown_blocks`：
    标题/列表/代码块/链接/引用都有），头部两个小按钮 `渲染 | 源码`，当前模式是"亮着的标签"而不是按钮。
    实测：渲染下 h1 = "预览测试文档"，点源码显示 `# 预览测试文档`，再点渲染回来。
  - **二进制/图片不再按文本打开**：`project_read_file` 先按**内容**用 `stb_image` sniff 认图片
    （PNG/JPEG/GIF/WebP/BMP…；SVG/ICO/AVIF 按扩展名），返回 `kind=image` + `dataUri`（12MB 上限，
    浏览器直接画，无额外请求）；含 NUL 且不是 UTF-16（按奇偶校验）的是 `kind=binary`，只给大小；
    其余才是 `kind=text`。侧栏按 kind 分别渲染 `<img>` / "二进制文件，没有可显示的文字。 (264 B)" /
    代码视图。实测：`dot.png` → `data:image/png;base64,`，`naturalWidth=1`；`blob.bin` → 二进制提示。
  - **测试**：413 用例 3889 断言全过；web / webview 双变体编译通过。新增
    `project_read_file_kinds`（文本/图片/二进制/UTF-16 四态）、`find_files` 跳过 `build/` 并给出 note、
    `edit_file` 拒绝 33MB 文件、`uiapp_interim_moves_narration_out_of_the_answer`（解说从答案移到工作日志，
    turn.done 后折叠）；两个工作过程折叠用例改成"运行中展开、结束后折叠、点开可见"。

<95>(SOLVED): webview 桥接泄漏与逐节点 IPC（"界面卡死/内存暴涨"的排查结论）+ 侧栏不递归 + 工作过程改成"工具在前、解说与思考在后" + 子代理记录补上工具名 + todo 悬浮清单。
  - **"放了一晚，电脑直接重启了"的排查**：事件日志给出的是 **BugCheck 0x0000010E
    (VIDEO_MEMORY_MANAGEMENT_INTERNAL)**——显卡驱动崩溃，不是用户态 OOM（16:50 异常关机，17:02 重启，
    dump 在 `C:\Windows\Minidump\110326-22062-0*.dmp`）。同一天 9:03/9:08 的 Resource-Exhaustion
    (事件 2004) 把 `flashsight-webview.exe` 报成占用 52GB，但实测那是**固定保留的虚拟地址空间，不是增长**：
    • 一个 hello-world 级 mingw 程序在这台机器上就保留 4.39GB（4GB 的那块区域，工具链/加载器行为）；
    • WebView2 再保留 32GB + 16GB 两个 cage，进程启动即 56GB VirtualSize，私有提交只有 7.5MB；
    • 40 轮工具调用的压力回合里，本进程私有内存 12→16MB、WebView2 进程组 ~2.0→2.1GB 后回落，
      回合结束空闲 60 秒内存不涨。
    结论：2004 事件里的 52GB 是 VirtualSize（Windows 按它报"最大虚拟内存使用者"），不是提交；真正的重启
    是 GPU 驱动 bugcheck。驱动该更新；若再复发，minidump 能指认具体驱动。**但这不代表桥接没问题**：
    同一轮排查找到了三处真实的泄漏，全部修掉：
    ① 每个弹窗都在 document 上装一个 mousedown 监听（`outside`），弹窗节点销毁后监听永远不删，
      闭包把**整棵游离的弹窗子树**留在内存里。现在只有一个全局 dismiss 分发器 + 节点表，断开即清。
    ② `bind_outside_click` 在弹窗/模态开着时**每次 render 都调用**（每秒几十次），旧实现只覆盖
      `node → id` 映射，旧回调永远留在 `outside_cbs_` 里。现在同一 popup 先删后加，`remove()` 时一并清理。
    ③ C++ 的 `Slot` 在 `remove()` 后只置 `valid=false`，**文本/值字符串还留着**；转录里被替换掉的
      节点把最后一版内容带进坟墓。现在直接重置整个 Slot。
    另外把"沟通"本身收紧了：每个 DOM 写原来都是一次 `ExecuteScript`（COM 跨进程），一帧成百上千次；
    现在 adapter 把一帧的结构/内容写**攒成一个脚本**，`UiApp::render` 在 patch 后 flush 一次，
    动效/焦点/滚动这些必须立即可见的操作仍然即时发。
  - **"文件夹搜索为什么递归，如果是侧边栏就过分了"**：全代码库只有一处 `recursive_directory_iterator`，
    就是模型工具 `find_files`（上一批已经加了生成目录跳过 + 单文件 4MB / 整次 64MB 预算）；
    侧栏文件面板走 `project_files`，是**单层 listdir**、最多 2000 项，永远不会递归。可以放心。
  - **工作过程顺序**：之前是"工具块和过程解说交替"。真实的一轮绝大多数是**一堆工具调用 + 一段话**，
    所以现在顺序固定为：工具块（按调用顺序）→ 过程解说 → 思考（最后）。折叠头不变。
  - **子代理记录**：一条 assistant 消息同时有文字和工具调用时，以前只在"没有文字"的情况下显示工具名；
    现在两者都显示（文字 + `工具调用: a, b`）。
  - **todo 悬浮清单**：`todo.updated` 以前只弹一个 toast，UI 根本没有列表——模型能加待办但什么都看不到。
    现在：右上角悬浮"任务清单"（进度 done/total、逐项状态、收起按钮），与左侧"任务规划"浮窗分开；
    composer 里只在存在清单时出现"清单"按钮（可重新打开）；事件实时更新，重开会话由新接口
    `session.todos` 从宿主内存取回（父会话与子代理的清单按 owner 隔离，不会被子代理覆盖）。
  - **测试**：416 用例 3907 断言全过；web / webview 双变体编译通过。新增
    `uiapp_todo_panel_shows_model_checklist`、`uiapp_work_process_puts_thinking_last`（工具 < 解说 < 思考）、
    `uiapp_subagent_transcript_shows_tools_with_text`。实测：浏览器与原生窗口各跑一轮真实回合，
    清单浮窗出现/收起/重开/重载后仍在，工作过程里工具块在解说之前。

<96>(SOLVED): 工作过程顺序按"到达顺序"回正（不是分组）+ 隐藏窗口停止绘制并暂停全部动画 + 流式重绘降到 60ms + 运行中只保留最近 120 步。
  - **顺序是我理解错了，改正**：上一批把"工具 / 解说 / 思考"分成了三段，这不是要的。要的是**一条按到达顺序的流**：
    每轮的过程解说（interim）照旧出现在它那轮工具调用**之前**（loop 就是轮末先发解说、再执行工具），
    思考没有新的工具调用可挂时**追加在流的末尾**，之后若再来工具调用就自然落在它下面。
    现在的实现就是这个顺序（`uiapp_work_process_puts_thinking_last` 断言 解说 < 工具 < 思考）。
    附注：目前 provider 层只**发送** reasoning effort，并不解析响应里的 reasoning 内容，所以界面上看到的"思考"
    其实是模型轮末的过程解说；将来一旦按轮捕获真正的 reasoning，它也会插进同一条流里对应的位置，而不是单独成块。
  - **"跑了任务才卡死、平时不卡"→ 把常驻的 UI 负载压下来**（显卡驱动是否背锅无法在本地证实，但下面这些是
    运行一整晚时真实存在的持续负载，且与"只有跑任务才出事"完全吻合）：
    ① **隐藏/最小化/被完全遮挡时不再绘制**。`IDom::hidden()`（wasm 读 `document.hidden`，webview 由
    可见性事件推送镜像）为真时：`paint()` 只记一个待绘标志、`pump()` 直接返回；`stream.delta` / `tool.*` /
    `interim` 全部走这条路径。以前窗口在后台时仍然按 16fps 全量重建转录并跨越进程发 DOM 写——
    跑一整晚就是几个小时的纯浪费。安全阀：隐藏时仍然 **0.5Hz** 补一帧，防止某个引擎一直报 hidden 把界面冻住。
    ② **CSS 动画在隐藏时全部暂停**（`html.fs-asleep *` 的 `animation-play-state: paused`）：spinner、光标闪烁、
    转圈之类本来会在 GPU 上以 60fps 一直转，看不见却一直消耗显存/合成资源——iGPU 上这正是"纹理无限增生"的
    常见来源。两个 adapter 都在 visibilitychange 时切换这个 class。
    ③ **流式重绘从 25ms 提到 60ms**（约 16fps）：每次重绘都要重新解析增长的 markdown、重新栅格化整列；
    一整轮下来少 4 倍的 DOM/GPU 工作量。文本观感没有区别。
    ④ **运行中只渲染最近 120 步**工具块，更早的显示一行"更早的 N 步已省略（本轮结束后可展开查看全部）"。
    以前一轮几百个工具调用时，转录越来越高、每帧都要走/画整列；结束后（streaming=false）仍然全量渲染，不丢内容。
  - **测试**：416 用例 3907 断言全过；web / webview 双变体编译通过。隐藏窗口行为实测（CDP 覆写 `document.hidden`
    + visibilitychange）：`fs-asleep` 类正确切换，后台时 DOM 只以 0.5Hz 更新（3 秒内只有一次批量变化），
    恢复可见后立刻追平。原生窗口实测：清单浮窗、工具/解说顺序（解说在工具前）都正常。
  - **没改的部分**：<95> 里的三处桥接泄漏修复与逐帧批量 IPC 保持有效（它们才是"跑久了内存涨"的实证部分）；
    如果你的机器还复发，请把 `C:\Windows\Minidump` 里最新的 dmp 与现场的 GPU/驱动版本给我——0x10E 的
    dump 能直接指认是哪个驱动模块挂的。

<97>(SOLVED): 回退 <96> 的渲染侧优化（浏览器本来就会做）+ 修掉 patch_children 的 O(n²)（历史大了进入卡顿/复杂度爆）+ 回到会话时"跟着底部"而不是旧像素 + 桌面事件队列加上限 + 资源日志。
  - **"渲染相关的优化是无用功"**：接受。隐藏窗口的浏览器侧节流/动画暂停本来就由引擎处理，本次把 <96> 里那几条
    回退干净：`IDom::hidden()`、`paint()`/`hidden_paint_`、`fs-asleep` CSS、60ms 重绘地板（回到 25ms）、运行中
    120 步上限。工作过程的**到达顺序**保持（解说在工具前、思考追加末尾）——那是显示语义，不是性能优化。
  - **"history 大了进入会卡，然后在顶上而不是底下"——两个真 bug**：
    ① `patch_children` 里三处"每个旧子节点扫一遍所有新子节点"的嵌套循环（丢节点、子序列判断、插入新节点），
    每次重绘对**任何**子列表都是 O(n²)，转录就是最大的那个。几百条消息时每次重绘要做上百万次节点比较，
    所以"百元素级别的 DOM 不应该卡"——卡的不是 DOM，是这三段。改成 `unordered_set` 按节点 id 判断成员，O(n)。
    实测（600 回合的会话，30 条/页）：打开 356ms 且落在底部；连续加载 8 页旧历史，每页 61→78ms（30→270 条消息，
    基本线性）；270 条消息时展开一个折叠块的重绘 4ms。
    ② 回到会话时，代码无条件用**上次保存的像素偏移**恢复，哪怕离开时视图是"钉在底部"的（`hist_follow`），
    于是会话变长后那个旧偏移只落在中部甚至顶部。现在 `hist_follow` 为真（或没有有效偏移）就滚到底部，
    只有"上次是滚动到中间看历史"才恢复精确偏移。原有同帧恢复用例补上 `hist_follow=false` 的语义。
  - **工具 ↔ UI 的衔接（你怀疑的那条缝）**：先回答 HTTP 的问题——**桌面/webview 版不走 HTTP**：
    `main.cpp` 在 `FS_UI_WEBVIEW` 分支直接 `run_webview_ui`，UI 用进程内 `NativeBackend` 调 FsApi，
    根本不启动本地服务器（注释也写明了这是刻意设计）。所以"某次工具调用把端口挤占"在原生版不成立；
    端口/SSE 只存在于浏览器版。**但这条缝上确实有一个无界缓冲**：`NativeBackend::queue_` 没有任何上限，
    UI 线程一旦被卡住（例如上面那个 O(n²) 让一次重绘变成几秒），工具事件会一直堆积，长得和"工具调用触发的
    内存增长"一模一样。现在队列上限 20000 条，超限时丢弃**最旧的展示类事件**（stream.delta / tool.update /
    interim / terminal.output / subagent.started——回合本身由 loop 落盘，turn.done/切页会重读），
    生命周期事件（turn.done、loop.interrupted、permission.prompt…）永不丢。
  - **下一次复发的取证**：设置 `FS_RES_LOG=1` 启动原生版，每 30 秒往 `%TEMP%\flashsight_res.log` 写一行：
    msgs/tools/msg_nodes/views/details/subagents/ops/todos/terms/term_bytes + `handles=` + `pump_gap_ms=`
    （pump_gap 是两次 50ms tick 的实际间隔，UI 线程被卡住时会暴涨）。下次真出现"跑任务卡死"，
    这行日志会直接指出是句柄涨、某个容器涨，还是 UI 线程被堵住——不用再猜。
  - **测试**：416 用例 3907 断言全过；web / webview 双变体编译通过；大历史实测数据见上。

<98>(SOLVED): 过程解说按"到达时的工具数"落位（多工具轮次不再错位）+ 规划与清单合一 + 子代理引导移到详情页且不再穿透 + 打开记录不再触发折叠 + 历史按消息锚定 + 慢事件日志。
  - **"新的思考跑到了上一个工具下面"的真因**：`assistant.interim` 是"每轮一条"，但一轮里可以有多个工具调用；UI 用
    "解说序号 <= 工具序号"来摆放，于是第二轮的解说明明应该排在第一批工具之后，却被放到了第 2 个工具之前。
    现在循环在发解说时带上**当时已经执行过的工具数**（`assistant.interim {tools:N}`），落盘时存成 `interimAfter`
    数组；UI 按"解说 k 排在工具 anchor[k] 之前"摆放，旧数据没有 `interimAfter` 时退回按序号（兼容）。
    这样就是纯到达顺序：一批工具、解说、又一批工具，解说永远紧跟在它到达时的位置。
  - **规划与 todo 重复**：两者都在回答"这轮在做什么"，同时显示等于把同一批步骤画两遍。现在**只留一个视图**：
    新的规划到达就清掉清单；新的清单到达就关掉规划浮窗（后端的规划树不动，auto-continue 不受影响）；
    composer 里也只有一个按钮（有清单显示"清单"，否则显示"规划"）。
  - **子代理引导移到详情页**：概览卡片是"点一下打开记录"的整卡点击区，输入框放在卡片里必然被点击穿透跳走。
    现在概览卡片只留"取消"，`引导/追加任务` 输入框在**详情页**（运行中用 steer，已结束用 followUp；
    草稿按子代理 id 存在详情状态里）。
  - **卡片内控件不再冒泡**：两个 DOM 适配器在 click 监听里增加一条通用规则——`evt.target.closest('[data-no-bubble]')`
    就直接返回，不执行所在行的点击。已标记：工具行的"打开记录"（它以前同时打开记录并翻转折叠，点一次变两次）、
    子代理卡片的操作区。
  - **历史滚动锚定**：新增 `IDom::scroll_anchor()`（拖动时算出"顶部是哪条消息"）和 `scroll_into_view()`；
    离开会话时同时保存顶部消息的 uid（`hist_anchor_uid`），回来时优先把**这条消息**滚到顶部，像素偏移只作兜底；
    钉在底部时仍然直接到底。浏览器端可同步测量，原生端只有一帧滞后的镜像，因此原生回退到像素（已注明）。
    实测（600 回合会话）：滚到 3522px（顶部是第 13 条消息），切到别的会话再切回来，位置仍是 3522 / 第 13 条。
    **历史分页策略没变**：首选页 30 条，往上滚按需加载，之前的 O(n²) 修复才是"打开就卡"的原因。
  - **慢事件日志**（针对"发出工具调用后立马卡死"）：`FS_RES_LOG=1` 时，任何一次后端事件处理（含其中的重绘）
    超过 500ms 就写一行 `[slow] event=tool.update ms=…` 到 `%TEMP%\flashsight_res.log`，和周期性的
    `[res]`（容器大小 / handles / pump_gap）配合。下次再"刚发完工具调用就卡"，日志会直接指认是哪个事件、
    卡了多久——这比继续猜测可靠。
  - **测试**：419 用例 3922 断言全过；web / webview 双变体编译通过。新增
    `uiapp_work_notes_follow_tool_anchors`（3 个工具 + 锚点 0/2 的解说位置）、`uiapp_plan_and_todo_are_one_checklist`、
    `uiapp_inner_controls_do_not_bubble`。

<99>(SOLVED): 卡片内控件穿透的二次修正 + 挂起改成"跑完当前回合再停"（中止才是硬停）+ edit_file 允许省略 start（不再退回 file_regex）+ 完成的清单直接删掉 + 会话行按 pid 刷、切会话补一次状态刷新 + 慢渲染日志、资源日志默认开启。
  - **子代理记录又点不开**：上一批的 `data-no-bubble` 守卫用了 `closest()`，而 `closest` **也匹配监听绑定的那个元素
    自己**，于是"打开记录"按钮自己的点击被自己吞掉（既没穿透也没动作）。现在只有"祖先带有标记"时才拦截
    （`marked !== el`），按钮自己的处理器正常执行，折叠也不会被连带翻转。
  - **挂起语义**：按你的定义改成分开的两件事——**挂起 = 优雅**：正在进行的那次模型调用照常跑完，它请求的工具照常
    执行，然后在**下一次模型调用之前**停下（`loop.state suspended`，可以恢复）；**中止 = 硬停**：新增
    `session.interrupt`（`AgentLoop::abort_requested_`），它通过 cancel 谓词真正中断流式回复并把已产生的部分
    停车。之前两者用同一个 suspend 标志，所以点挂起看起来像"中断了当前轮"。
  - **edit_file 总是失败**：模型发的是 `{"end":7051,"expect":...,"newText":...}`（**没有 start**），旧校验把缺省
    当成 0 直接拒绝（`bad line range: start=0 end=7051`），模型于是退回用不一定安全的 `file_regex` 改文件。
    现在：省略 start 时按 `expect` 的行数把起点推出来（start = end - lines(expect) + 1；没有 expect 就只改 end
    那一行），`start: 0` 视为第 1 行；schema 里 start 不再是必填，描述写清了这三种情形。
  - **完成的清单**：全部打勾后清单**直接从宿主里删掉**（`todo_op` 检查后 pending==0 就 clear 并广播空列表），
    重进会话不会再把它重建出来；`erase` 之前漏发事件，顺手补上。
  - **"打开别的会话，这个会话的运行中标识就没了"**：切会话后补一次 `mark_sessions_dirty()`，让侧栏立刻按后端的
    `running` 重读（之前要等某个事件碰巧触发刷新）。同时把 `load_sessions` 的回包**按 pid 定位项目**：项目列表按
    活跃度重排过，用旧下标写回会把 A 项目的会话行写到 B 项目上（行上的圆点/运行状态错位）。
  - **卡死取证**：`%TEMP%\flashsight_res.log` 之前只有我测试时的那几行——你那轮没有开 `FS_RES_LOG`，所以没有数据。
    现在原生版**默认就写**这个日志（`FS_RES_LOG=0` 可关）：每 30 秒一行容器大小/handles/pump_gap；并新增
    **慢渲染日志**——任何一次 `render()` 超过 200ms 打印 `[slow] render ms=… msgs=… tab=…`（wasm 里进浏览器
    控制台，原生进 stderr）。下次"点一下卡几秒"，日志会直接说是哪个渲染、多少条消息。
    另外这次针对 600 回合的大会话实测：打开一页（30 条）渲染没有出现 >200ms 的条目，说明 <97> 的 O(n²) 修复
    之后单页渲染是快的；真正需要取证的是你那次的具体现场。
  - **测试**：421 用例 3939 断言全过；web / webview 双变体编译通过。新增
    `loop_suspend_finishes_round_then_parks`（挂起后本轮工具照跑、不发下一次模型调用）、
    `edit_file_derives_missing_start`（省略 start / start=0 两种写法）；两个"停止生成"用例改用 `interrupt()`。

<100>(SOLVED): 卡死的真因（634 个工具行每帧全量重建 → 记忆化）+ edit_file 的 CRLF 校验与默认 LF + 发送后迟迟看不到内容。
  - **资源日志抓到了现场**：`[res] … msgs=22 tools=634 msg_nodes=22 … terms=195 handles=≈470-536
    pump_gap_ms≈60`。也就是**一条消息里 634 个工具块**（WhaleUI 那类任务），而界面在流式输出时每个增量/每个
    工具事件都会重建整条消息 = 复制 634 行的字符串、新建 634 个子树；一轮下来是 O(N²) 的分配。这正是"发完工具
    调用就卡、界面卡死但后台还在跑、那一轮最后跑完"的机制——不是显卡驱动。`pump_gap_ms` 一直 ≈60ms 说明发生
    卡顿时 UI 线程是在做这堆重建，而不是被锁住。
    修复：新增 `tool_block_cached`，按 `(key, rev, 输入/返回/摘要长度, 展开态, running/ok)` 记忆化工具行——没变
    的行直接复用同一个 VNode（和聊天消息的 msg_nodes_ 同一思路），缓存上限 4000 条；`ToolBlock::rev` 在
    tool.start/tool.update 时递增，保证变了的行会重建。每帧成本从"O(工具数) 的字符串拷贝"降为"未变的行 O(1)"。
  - **edit_file 的 "expected text does not match lines 1797-1798"**：`actual` 用**文件自己的换行**拼出来（CRLF），
    模型的 `expect` 用的是 `\n` —— 内容一样也判不符。现在两边**都去掉 \r 再比较**；并且按你的要求把
    `lineEnding` 的默认值从 `preserve` 改成 **`lf`**（编辑/写入后统一成 `\n`，`crlf`/`preserve` 仍可显式指定），
    原来的 `edit_preserves_crlf` 用例改成 `edit_normalises_crlf_to_lf`。
  - **"输入后迟迟看不到结果（包括加载中）"**：发送后用户消息与"运行中"都在 UI 线程上，而 UI 线程正卡在上一轮
    几百行的重建里，所以连"加载中"都出不来。工具行记忆化之后这条路径恢复即时（同样是上面那个根因）。
  - **两个实例同时跑**（webview 跑 WhaleUI、web 跑 WhaleJS）：它们各自是独立进程，但**共用同一个数据根**时，
    同一项目/会话的文件会被两个进程同时读写，持久化会互相覆盖（状态"看起来不对"）。建议两个实例用不同的
    `--root`，或者只开一个。这一条是使用约束，不是代码 bug。
  - **测试**：423 用例 3946 断言全过；web / webview 双变体编译通过。新增
    `uiapp_tool_rows_are_memoised`（未变行返回同一 VNode、变了重建）、
    `edit_file_matches_expect_across_line_endings`（CRLF 文件 + \n expect 能改，且默认写成 LF）。

<101>(SOLVED): 工作过程第二次点不开 / 继续按钮失效（缓存 VNode 复用已销毁的子树）+ 假终端 / 空转 / 首字等待 / 清单与规划。
  - **工作过程只能点开一次**：`tool_block_cached` 记忆化的行在收起时，退场动画会把 DOM 节点从页面移除，而缓存仍持有
    那些 VNode —— diff 以为它们还挂在树上。第二次展开时 patch 的目标已经不在文档里，事件与可见性都失效。修复：新增
    `ChatMsg::work_seq` 作"折叠代"，收起/展开以及 `turn.done`/`loop.interrupted` 都递增；缓存键带代，外层行节点也
    key 成 `"tb:<gen>:<key>"`，旧代的节点不再被复用。**对已移除节点做 patch/绑定在适配器里是未定义行为，这也是闪退的
    候选机制**（闪退本身没复现，这次一并消除该来源；原生版仍默认写资源日志，下次能直接抓现场）。
  - **"任务出错"里的继续点不动**：`continue_task("")` 之前会追加一条空 user 消息、或要求 loop 处于 parked。现在空指令
    直接接着已保存的 transcript 继续（不插空消息），并清掉中止/挂起标志；什么都没保存时才报 "nothing to continue"。
  - **任务规划与清单重复**：新增 todo 时宿主清空 plan（`plan_get`/`plan_update` 写空 + 广播 `plan.update` 空）；界面只
    在没有清单时显示规划浮窗，输入框只留一个控件（有清单显示"清单"，否则"规划"）。完成全部勾选后清单本身也从宿主删掉。
  - **侧栏/清单不响应式**：切标签页后补一次 `load_side_tab(active_side)`（之前只重载会话，侧栏停在旧数据）；
    `mark_sessions_dirty()` 让会话行按后端 `running` 立刻重读；`load_sessions` 回包**按 pid 定位项目**（项目按活跃度
    重排，用旧下标写回会把 A 的会话行写到 B 上）。
  - **自动轮 5 次空转**：`should_auto_continue` 现在清单优先于规划——有未完成项才继续，全完成就停；清单为空时只有模型
    显式要求继续才继续。状态提示跟着循环状态实时刷新。
  - **首字延时像卡死**：发送时立刻插入一条 streaming 占位消息，正文渲染 `.turn-waiting`（spinner + "正在请求模型…"），
    不再等第一个 token 才有反馈；文案入 i18n 表。
  - **WhaleUI 的 195 个假终端**：227 条记录里只有 1 个日志文件。前台命令的输出在 `spawn` 之后立刻被 drain，而日志路径
    是稍后才绑定的，那段输出没有落盘。现在 `TerminalManager::set_log_path_fn` 在 `create` 时就绑定（子进程启动前），
    `append_terminal_log` 在映射里找不到时回查注册表；`terminal_list` 只列活着或日志确实存在的记录（带 `logExists`），
    最多 50 条、新的在前。点进去不会再是空文件。
  - **edit_file 的 "expected text does not match"**：`expect` 两侧去 `\r` 已在 <100> 做了；这批把 schema 的 `required`
    改成 `["newText"]`（`start` 可选，缺省时按 `expect` 行数推导），模型不必再退回不安全的 `file_regex`。
  - **测试**：424 用例 3957 断言全过；web / webview 双变体编译通过。新增 `loop_continue_with_empty_guidance`（出错后
    空指令继续、不插空 user 消息）、更新 `uiapp_tool_rows_are_memoised`（缓存带折叠代）等 3 个受占位消息/缓存影响的用例。

<102>(SOLVED): 事件归属改为框架级 + 规划=文档（三层：任务列表/规划/工作流）+ 首字读秒 + 中转重试 + 缓存命中 + 补全/回底。
  - **事件穿透改成框架规则**：适配器里每个元素自己的 click/dblclick/contextmenu 处理完就 `stopPropagation()`——
    事件属于**最内层**的 handler，外层行不再收到。删掉了所有 `data-no-bubble` 标记和两处 `closest` 补丁：以后任何
    新控件都不需要记得加属性。真实浏览器里工作过程连续开/关 4 次：open/closed/open/closed。
  - **删掉规划小窗**：没有 `plan-float`、没有输入框里的"规划"按钮、没有 `ui:plan` 命令、没有 plan 步骤解析和评审面板。
    唯一进度视图是任务清单悬浮窗（右侧）。
  - **规划重新定义为文档（addon2 三层）**：任务列表(todo，本轮) - 规划(plan，**文档** `<project>/.flashsight/plans/<sid>.md`)
    - 工作流(workflow，流水线，用户批准)。`plan_update` 写整篇 markdown（`content`）或只改状态（`status: active|done`）；
    `plan_get` 返回 {exists,status,content,path}。模型每轮从文档拆出 todo，全部做完必须 `status="done"`；运行时提示块
    只报"文档存在/路径/状态"，正文让模型用 plan_get 读（文档上限 64KB）。删掉了"清单优先于规划""新的清单清空规划"
    这类概念混淆的逻辑——两者是不同层，不是同一个东西的两个视图。
  - **自动轮**：有未完成 todo → 继续；否则规划文档仍 active（且正文非空）→ 继续；都没有 → 停。另加空转保护：
    **自动轮如果一次工具都没执行，就不再继续**（这正是"连续 5 次模型输出当前没有新的指令"的机制）。
  - **首字等待**：删掉"正在请求模型…"这行新提示；发送时立刻插入 streaming 占位消息，消息自带的 spinner+读秒
    立即出现（`turn-live`，每秒刷新）。回车发送后强制贴底（`hist_follow=true` + send 后 scroll_to_bottom）。
  - **中转断联重试**：`RetryPolicy` 默认改为 3 次重试、1s/2s/4s 退避（网络/超时/5xx）；**已经吐出内容的那次不重试**
    （重放会把屏幕上已有的半截回答再打一遍）。流式 usage 的缓存字段补全：DeepSeek `prompt_cache_hit_tokens`、
    OpenAI `prompt_tokens_details.cached_tokens`、Anthropic `cache_read_input_tokens`；只有 miss 时用
    prompt - miss 推导。新增 `cacheKnown` 区分"命中 0%"和"供应商没报"—；命中率 = 缓存输入 / 总输入。
  - **路径补全**：无匹配就关闭（空菜单不再挡在输入框上）；回包只在"当前仍在输入同一个片段"时生效，写完路径去编辑
    别的文字时迟到的回包不会再把它弹出来。
  - **测试**：426 用例 3954 断言全过；web / webview 双变体编译通过。新增
    `provider_stream_usage_reports_cache_hit`（DeepSeek/OpenAI 两种字段）、`provider_retries_a_dropped_relay`（3 次内
    成功）、`provider_does_not_retry_after_partial_output`（已输出不重试）、`loop_auto_stops_after_empty_round`；
    规划用例改为文档形态（写入/只改状态/驳回删文件）。

<103>(SOLVED): 工具行展开/折叠后点不开 + todo 按钮常驻 + 会话打开被余额探测锁住 + 模型用 todo 的短提醒 + 用量环形图/总 token + 供应商面板去壳。
  - **工具行第二次点不开**：工作过程本身已经用折叠代修好了，但**单个工具块**的展开体是另一处：`tool_block_cached`
    把"展开态"的节点缓存起来，折叠时详细体被移出 DOM，再展开又命中那个旧节点，diff 以为 body 还在——于是点不动。
    修复：每次展开/折叠都把该工具行的 `rev` 递增（它本来就是缓存键的一部分，`ToolBlock::rev` 的契约是"每次变更都
    递增"）。浏览器里对同一行连续点 4 次：open/closed/open/closed。
  - **todo 按钮**：之前只在有清单时出现，删掉规划按钮后就彻底找不到了。现在"清单"按钮**常驻**输入框工具栏；空列表
    点开显示"暂无任务（模型会在这里记录本轮步骤）"。
  - **进入会话很久才加载**：真因是后端锁。`UiApi::call` 给除 `session.send`/`tool.invoke` 外的所有方法都套了全局
    递归锁，而 `provider.balances` 会并行探测每个供应商、最长 5 秒——这段时间里 `session.info`/`session.turns`
    全被堵住，界面先显示空会话。修复：`provider.balances` 也从全局锁排除，它只在**快照供应商列表**的瞬间取锁，
    网络探测在锁外跑。实测：余额探测进行中（1.26s），`session.turns` 34ms 返回。
  - **模型不积极建 todo**：长上下文里系统提示会被"遗忘"。运行时块**每轮**追加一行极短提醒：
    "记住以上准则：多步任务先用 todo_add 记步骤，完成一项就 todo_check；长任务维护规划文档。"（中英双语）
  - **用量统计**：按模型从条形改成**环状图**（conic-gradient 圆环 + 中心总 tokens + 右侧图例和百分比，超过 7 个
    模型合并为"其他"）；顶部指标新增"总 tokens"；表格的"缓存"列换成"总 tokens"（输入+输出）。
  - **供应商页**：模型列表外面那层带边框/背景/内边距的卡片 div 和小标题"模型"都去掉了，列表现在和表单/其他表格
    同一宽度、同一基线，不再"小一圈"。
  - **测试**：426 用例 3971 断言全过；web / webview 双变体编译通过。浏览器验证：工具行 4 次开合、清单按钮存在、
    环形图 1 个 + 2 条图例 + 中心 4.0K、表头为 模型/输入/输出/总 tokens/成本（无缓存列）。

<104>(SOLVED): 工具行展开态缓存重开失效 + 用量页余额/额度删除 + 准则单行 SYSTEM 引用 + todo 会话级持久化 + 失败红点按已读持久化 + 侧栏标签中键关闭/拖拽落点。
  - **工具行"展开→折叠→再展开"点不开**：`tool_block_cached` 把展开态的节点也缓存了，而折叠时详细体已经从 DOM 移走；
    第三次展开命中旧节点，diff 以为 body 还在。修复：展开/折叠时把该工具行的 `rev` 也递增（缓存键的一部分），
    每代节点独立。浏览器里同一行连点 4 次：open/closed/open/closed。
  - **余额 / 额度**：从用量页和侧栏一起删掉（连同自动探测），之前那句"小一圈"就是 `.usage-balances` 的 12px 内边距
    把卡片缩进了一圈。现在用量页只有：总计指标（含总 tokens）、按天、按模型环形图、明细表。
  - **准则提醒**：运行时块每轮只加一行 `SYSTEM: 记住上面的【准则】。`（英文 `[RULES]`），指向全局系统提示里带标记
    的一节；不再复述 todo 的具体做法（可能不止 todo，而且两处容易漂移）。
  - **todo 跟随会话**：查满最后一项不再清空列表；列表写 `<session>/todos.json`，重启后 `session.todos` 仍能加载
    （实测：跑一轮 todo_add → 杀进程重启 → 列表仍在）。本轮 todo 在运行时块里仍然逐项列出，全部完成也保留。
  - **失败红点重启后复活**：已读时间以前只存在 UI 进程的内存 map 里。现在写 `<session>/seen.at`（独立小文件，**不碰
    full.json 的 mtime**——红点比较的就是 mtime 与已读时间），`session.list` 返回 `seenAt`，UI 启动时用它初始化；
    打开会话/回合在眼前结束/中断都会写。新增 `addon2_session_seen_survives_restart`（写标记后 updatedAt 不变）。
  - **侧栏标签**：中键关闭（新增框架事件 `kAuxClick`，三套适配器都映射 auxclick 且只认 button=1）；拖拽排序改为
    **按落点在目标标签的左/右半边决定 before/after**——原实现 `ti-1` 让相邻两个标签互拖变成 no-op，所以看起来
    "拖不动"。换行溢出保持原样。浏览器实测：把"用量"拖到"子代理"右半边，顺序变为 [子代理, 用量]；中键关闭后
    只剩 [用量]。上层标签条同样支持中键关闭。
  - **测试**：427 用例 3960 断言全过；web / webview 双变体编译通过。

<105>(SOLVED): 运行中标识被本地状态覆盖 + 继续按钮阻塞/不消失 + 切页面后工具点不开（缓存跨页复用）+ 余额区块恢复并对齐 + 结尾时间/读秒/可选中。
  - **项目列表"运行中"又没了**：`live_session_row` 对当前会话执行 `row.running = 本地 turn_running`，把后端快照里的
    running **覆盖成 false**——从项目列表打开一个正在跑的会话时（UI 没看到 turn.started）就显示空闲。现在本地活状态
    只能**加**（true），不能抹掉后端事实；只有挂起/中断才置 false。这是"瞎耦合"的典型：本地缓存状态盖掉了权威状态。
  - **任务已中断 / 继续**：
    1. `session.continue`（和 `session.resume`）跑的是整轮任务，却没被排除在 UiApi 的全局后端锁之外——点一次继续，
       整个界面被锁到那一轮结束，所以"点很多次才好"。两者已加入排除列表（与 session.send 同规则）。
    2. 中止恰好落在工具轮边界时，loop 原来也发 `loop.state=suspended`，界面只出"恢复"不出"任务已中断"条。现在边界处
       区分 abort（发 `loop.interrupted`，带 resumable）与 suspend。
    3. 继续按钮点下即隐藏（乐观），失败再恢复原条；`loop.resumed` 也会清掉。实测：中断出现条 → 点继续 400ms 后条消失、
       回合已在跑、不再复现。
  - **切页面后工具调用点不开**：不是"绑定/preventDefault"的问题，而是缓存跨页复用：消息 VNode（`msg_nodes_`）和工具行
    （`tool_block_cached`）在切走时 DOM 已被整页移除，切回时把这些"自信还挂着"的旧节点塞进新页面，子节点不会被重新
    插入，事件自然不生效。`load_session` 现在先丢掉该会话的消息缓存并清空工具行缓存（`tool_block_cache_clear()`），
    重新构建页面。浏览器实测：打开工具 → 切到另一个会话 → 切回 → 再点同一条工具，详细体正常展开。
  - **余额/额度恢复**：上一批理解错了——要删的是"小一圈"的外层内边距，不是区块。用量页和侧栏的余额区块都恢复，
    `.usage-balances` 的 12px padding 去掉（只留顶部 12px），卡片与上表左边缘对齐（实测偏移 0px）。
  - **chat 结尾时间**：助手消息不再显示"刚刚"这个发送时间，只显示**结束时间** + 总时长（开始时间在工具调用里有）。
    中断时 `ended_at` 用当前时刻；`fmt_duration` 改为整秒（不再 0.1s）。
  - **那几个时间/状态文字不能选中**：`.msg .meta` 与 `.panel-tabs` 加 `user-select: none`；拖标签时 mousedown
    `preventDefault()`，避免拖动过程选中文字（这可能就是拖动"失败"的来源）。
  - **测试**：427 用例 3991 断言全过；web / webview 双变体编译通过。

<106>(SOLVED): 侧栏标签按文字也能拖 + 运行中状态卡死（后端权威+看门狗）+ 终端只留"标记/长时间"+ 项目卡片宽窄两种溢出策略。
  - **标签拖不动（鼠标在文字上）**：webview 适配器的 mousedown 只接受 `ev.target` 本身就是标签，按在标题文字（span）上直接
    return——所以"放文字上根本拖不了"。现在和 wasm 一样向上爬到 `.tab/.ptab` 再开始拖。选中文字的问题也继续由
    `preventDefault()` + `user-select:none` 兜住。
  - **会话一直显示"运行中"（实际早结束）**：状态是后端权威，但界面只在事件边沿刷新 session.list；漏一个事件，旧的
    running 就一直留着（上一批还改成"本地只能加 true"，放大了这个问题）。现在 pump 里有**看门狗**：只要还有任何一行
    看起来在跑，就每 2 秒重读一次 session.list 和当前会话 loopState；后端说结束了，圆点就自己灭。空闲时零开销。
  - **终端记录太多**：不再把所有模型命令都当记录。规则：用户终端 / 后台终端 / **有 label 的（标记过的）** / 运行 ≥60s
    的才保留；普通前台 `terminal_exec` 快速结束的，退出时删除日志并从列表过滤。`terminal_list` 应用同一规则（旧记录
    也一样）；打开会话不再预加载终端列表，只在终端抽屉打开/新建终端时读取（"不用默认把所有终端都加载上来"）。
  - **主页项目卡片**：左右两栏模式给会话列表固定最大高度（360px）内部滚动；上下两栏（窄容器）保持**展开**显示全部、
    由标题行整体折叠（默认展开）。浏览器实测：宽 360px/auto、窄 none/visible、窄下点头部整块收起、切回宽仍显示。
  - **测试**：427 用例 3995 断言全过（新增：未标记的快速命令不进列表）；web / webview 双变体编译通过。

<107>(FIXED): 标签拖出幽灵但松手不挪（webview 回调 id 传错）+ 项目卡片行被 flex 压缩。
  - **拖出来了、松手不挪**：和 span 不响应是两件事，都已修。webview 适配器的 `bind_tab_drag` 把回调 id 以 **int 0**
    传给 JS，而胶水层是从**字符串参数**里 `parseInt(a)` 取 id，于是 `el.__fsTabCb` 为空，`if (el.__fsTabCb)` 直接
    跳过了 drop 上报——所以幽灵跟着走、松手什么也不发生。另外它只有一个回调槽，第二个标签条（侧栏）会覆盖第一个
    （顶栏）的回调。现在按**标签条节点 id** 建回调表、id 走字符串参数、按 id 分发，两条标签条互不干扰。
    原生 WebView2 + CDP 实测：按在标签**文字**上拖动 → 幽灵出现 → 松手后 `[用量, 子代理]` 变为 `[子代理, 用量]`。
  - **span 不响应**（上一批）：mousedown 只认 target 本身是标签，现在向上爬到 `.tab/.ptab`；与上面的 drop 问题独立。
  - **项目卡片挤压**：会话行是 flex 子项，默认 `flex-shrink:1`，列表一多就被压扁（不是没设高度，是高度被 flex 压缩）。
    现在 `.proj-session { flex: 0 0 32px; min-height:32px }`，`.projects/.proj-card/.proj-sessions/.proj-right-head`
    都是 `flex:none`。实测 18 个会话：列表 clientHeight 360、scrollHeight 576、每行 32px（正常滚动，不压缩）。
  - **测试**：427 用例 3995 断言全过；web / webview 双变体编译通过。

<108>(FIXED/AUDIT): 清单未清空自动轮却停 + 自动轮状态残留（一直显示在跑）+ 模型级重试落地 + 继续/恢复的后端锁与模型同步。
  - **"清单没清空，自动轮却不继续"**：两个原因都修了。
    1. 批次 50 加的"空转保护"（自动轮没执行工具就停）把**清单未完成**驱动的续跑也压掉了。现在只有清单里没有未完成项
       （即仅靠规划文档续跑）时才应用该保护；有未完成 todo 就继续。新增用例
       `loop_auto_continues_with_open_todos_after_empty_round`（3 次模型调用 / 2 个自动轮）。
    2. 前端同步：清单以前只在打开会话时读一次，漏一个 `todo.updated` 就会显示"清单没清空"而宿主里已经完成——正是
       你怀疑的"状态没跟上"。现在 pump 里在回合运行中或清单面板打开时，每 2 秒重读 `session.todos`，UI 与循环看到
       的是同一份清单。
  - **"一直显示在跑，输入框却不在跑"**：自动轮的 `auto.next` 以前不设置 `turn_running`（输入框显示发送），而 `turn.done`
    又不清 `auto_round`——最后一个自动轮结束后圆点仍按 auto_round>0 一直转。现在：`auto.next` 置 `turn_running=true`；
    `turn.done` 清 `auto_round`（若还有下一轮，紧随其后的 auto.next 会重新置位）。`auto.halted` 的提示也改成显示真实原因
    （轮数上限 / 空转停止），不再统一写"达到轮数上限"。
  - **模型级重试**：`Settings > 模型 > 高级` 的 `retry.max` / `retry.backoffMs` 以前只是存下来，从未被读。现在 FsApi 解析
    出实际使用的 ModelEntry 后，把这两个值写进 ProviderRuntime 的 RetryPolicy（0 = 用全局默认）。
  - **后端锁审计**：`AgentHost::continue_task` 原来整轮都持有 host 递归锁，导致这一轮里 `todos_for` / `is_running` /
    session.list 富化全部被堵——正是前端状态同步要用的调用。现在和 `send()` 一样：锁只包住查 loop，回合在锁外跑。
  - **继续/恢复的模型同步**：以前续跑沿用 send 时捕获的模型，挂起期间换过的模型不生效。`session.continue` /
    `session.resume` 现在先读会话当前的 `sessionSettings.model` 再续跑。
  - **测试**：428 用例 3999 断言全过；web / webview 双变体编译通过。

<109>(FIXED): 切会话把上一个会话的输入渲染出来（聚焦控件不写 value）+ 切回滚动静默跳顶/底（恢复回显被当真）+ 清单改为事件广播+按需强制读取。
  - **别的会话的输入跑到当前会话**：不是全局列表——是 diff 的既有规则"聚焦中的控件不写 value"（防止打字时重渲染把光标打回）。
    切换会话时 composer 往往仍是焦点，新会话的状态是空/另一段草稿，但 DOM 里还留着上一个会话的文字。新增
    `sync_composer_textarea()`：切会话（及新建会话清空输入）后，直接把当前会话的草稿值写进 textarea，绕过聚焦保护。
    实测：在 A 输入 SECRET-A → 切到 B，B 的输入框为空；切回 A，A 的草稿仍在。
  - **切回后一下顶部一下底部**：滚动边缘回调在恢复过程中也会收到"恢复自己造成的滚动"——内容还没铺开时被 clamp，可能同时是
    top/bottom；旧代码据此改写 `hist_follow`（甚至触发 load_older_history），于是先按记住的位置恢复、随后又被 follow 拉到
    底部。现在 `restoring_scroll_` 期间整个边缘回调直接忽略（恢复分支自己会显式设置 follow）。实测：滚到 1800 → 切走 →
    切回，位置稳定（内容高度不同 clamp 到 1519），2.5 秒后仍是 1519，不再跳。
  - **清单同步**：按你的意见去掉 2 秒轮询。`todo.updated` 仍然实时广播；另外在**打开会话、打开清单面板、回合结束**
    这三个时刻强制读取一次 `session.todos`，作为丢失广播时的兜底。
  - **测试**：428 用例 3999 断言全过；web / webview 双变体编译通过。滚动分页用例补了"先跑完恢复的两个 schedule tick"，
    与真实时间线一致。

<110>(PARTIAL): 运行中残留/思考档位/用量刷新/上下文档位可选 已修；供应商"新建"在原生 WebView 下仍打不开模板页（已取证，未根治）。
  - **没运行却显示运行中**：自动轮标记 `auto_round` 在两种"没有 turn.done"的结束路径上没清——`loop.interrupted` 与
    看门狗读到的 idle。现在两处都清；后端说不在跑，圆点就不会再按 auto_round>0 转。
  - **有默认思考档位却只有关/开**：`fill_model_defaults` 以前要求 levels 和 default 都为空才补；模型只有 default 时
    levels 一直空，而设置页保存路径会补，所以"去设置点一下保存就好了"。现在只要 levels 为空就按模型名补，并保证
    default 在档位表里；`model.allEnabled` 也对返回的副本跑一次（输入框的思考控件读的就是它），不必先保存。
  - **切页面用量不更新**：主页"用量统计"标签以前只在 `!usage_loaded` 时加载一次。现在每次进入都重读（侧栏用量仍按
    切会话重读）。
  - **模型上下文不再必填**：留空走默认 128000（后端 model.save 同样兜底），提示改为"API 获取优先，留空用默认"；
    不再因为空值拒绝保存。
  - **供应商"新建"（未修复，已定位到一半）**：点击确实触发（日志确认 state 变 creating=1、render 也以 26 个模板构建），
    但在原生 WebView2 里 `.pv-templates` 容器到了 DOM 而它的 26 个子卡片没有——同一页里更早的标题/提示能挂上。排除了
    分支子树键控、常驻页面 + display 切换、整页重建（重建时连 modal 层都不挂）几种方案；判断是 VDOM→webview 适配器
    在"新建深层子节点"这条路径上丢了尾部操作。为此加了可复现的取证：`FS_UI_EVENT_DEBUG=1` 时 EventBus 的缺失 handler、
    webview 的 fire、以及设置页构建状态都写到 `temp/fs_event.log`。下一次专门修这条通路。
  - **附带**：设置页 body 按 section 加 key（切 section 会重建，桥接下不再留旧 handler）；滚动分页用例适配了"恢复期间
    忽略滚动回显"。
  - **测试**：428 用例 4004 断言全过；web / webview 双变体编译通过。

<111>(FIXED): 供应商"新建"真因=webview 适配器的逐帧 DOM 批量写入丢尾；同名模型按供应商路由；统计按模型合并；API 模型列表自动补上下文。
  - **供应商"新建"（上一批未根治）找到真因了**：问题不在供应商页面，也不在事件。11 月 3 日给 webview 适配器加的
    "整帧 DOM 写入合成一条脚本"批量派发，在脚本尾部被截断/某条 op 失败时会**静默丢掉后面的所有操作**——模板页的
    标题和提示在前半段挂上了，26 张模板卡在后半段全没了，页面于是半残、控件也指向旧节点。10 月 23 日的版本在批量
    化之前，所以正常。现在 webview 回到**每条 DOM 操作一次 ExecuteScript**（与浏览器构建的同步 EM_JS 一致）。
    原生 WebView2 实测：设置→供应商→新建，26 个模板卡全部出现。
  - **两家供应商有同名模型却只能选第一家的**：供应商以前根本不是模型身份的一部分（`pick_model` 直接丢掉了
    provider_id）。现在端到端都带上：`sessionSettings.provider`、`session.send` 的 `providerId`、
    `ModelCallRequest.provider`、`AgentLoop::set_provider`；`call_model` 优先用它选连接（空才按名称回退）；
    继续/恢复也会重读 provider。模型选择器存 provider，模型可用性按 (provider, model) 判定，不一致就清空重选。
    新增 `addon2_session_model_provider_is_persisted`。
  - **用量统计按模型合并**：`usage.query` 新增 `byModel`（跨供应商按模型求和）；用量页的环图和明细表改用它，
    不再显示 `pr_xxx / model` 这种内部供应商 id。**路由仍然带供应商**，两件事分开。
  - **API 获取模型列表自动补上下文**：`/models` 大多不报上下文，以前就留 0，UI 要求手填。现在 `fetch_models`
    对每条模型跑 `fill_model_defaults`，其中新增 `default_context_for`：已知家族（gemini 1M、claude 200k、
    gpt-4.1/5 1M、o1/o3/o4 200k、deepseek 128k、qwen 131k、kimi 256k…）给保守值，其余默认 128000；用户仍可覆盖。
  - **测试**：429 用例 4012 断言全过；web / webview 双变体编译通过。原生窗口验证模板页 26 张卡。


<112>(FIXED): 选择器选中态按 (供应商, 模型) 判定 + 选完模型立即刷新思考档位 + 默认上下文改 1M 基线（低配关键词才降到 131K）+ Muse Spark 不再当成讯飞 Spark。
  - **同名模型两页都打勾、还总跳到第一家**：上一批只修了路由，选择页面仍按模型 id 判定选中，`open_model_picker` 也
    默认供应商列表第一家。现在选中态比较 PAIR（`st_.model_provider`），行 key 也带 provider（切换供应商页会重建行，
    不会复用上一家的 active 类）；打开选择器直接定位到当前模型所属供应商。老会话若没记住 provider，则按后端同样的
    解析顺序（第一条启用副本）标记，仍然只打一个勾。原生 WebView2 实测：从 DeepSeek 的 deepseek-flash 打开选择器
    落在 DeepSeek、1 个 active/1 个勾；切到 Happy API 后 0 active/0 勾；关掉重开又回到 DeepSeek。
  - **思考档位必须去供应商设置保存一遍才更新**：选择器里点模型只改了 `model`/`model_provider` 和会话设置，没有重读
    模型表；输入框的档位读的是 `model.allEnabled`，所以直到设置页保存模型（那里会调 `refresh_model_availability`）
    才变成新模型的档位。现在 `pick_model` 选完立即刷新，并校验当前档位：新档位表非空且旧值不在表里（例如从 medium
    换到 off/low/high）时清掉旧值，再选中模型自己的默认档，避免控件显示新默认而请求还带旧档位。档位表为空（模型表
    没有描述该模型）时保留会话记忆值，兼容旧行为。
  - **默认上下文太短**：`default_context_for` 从"按家族给保守值"（claude 200k / deepseek 128k / qwen 131k / 默认
    128k）改成 **1M 基线**（1048576），只有模型名的**独立 token** 明确是低配才降到 131072：mini / nano / flash /
    lite / small / tiny / turbo / haiku / air，以及 7b / 8b / 1.5b 这类裸参数量。按 token 匹配是为了避免 "minimax"
    被当成 "mini"；gemini 家族优先判 1M（Flash 也是 1M）。UI 保存模型不再把空值写成 128000，后端 model.save 也
    交给 `fill_model_defaults`，提示改为"留空按模型名自动匹配（默认 1M）"。实测 Happy API 列表：grok-4.7 / muse 等
    显示 1048K，agnes-2.5-flash / deepseek-v4.1-flash / glm-5.3-flash 等显示 131K。
  - **Meta 的 Muse Spark 被认成讯飞 Spark 星火**：`model_icon_for` 的品牌表里 "spark" 是裸子串，排在前面时把
    "muse_spark_..." 吞掉了。现在 Meta 规则包含 "muse"（UI 的 `brand_for_name` 同步），先于 spark 命中。另外，
    之前按旧逻辑落盘的 API 模型图标（from_api 且非 user_set）会在 `fill_model_defaults` 里按新规则重算，否则配置里
    的 "spark" 会一直留着；用户手改过的图标（user_set）不动。原生窗口实测两条 muse 模型都是 `mi-lobe-meta`。
  - **测试**：433 用例 4034 断言全过；web / webview 双变体编译通过。新增选择器 pair/打开定位、选模型刷新档位、
    1M 上下文基线、Muse 品牌与旧图标重算的用例。


<113>(FIXED): 双会话卡死真因=前台终端等待持锁 + wakeOnExit 从未接线；定时唤醒工具；模型上下文按 2026 资料重排；complete-step 阻塞派活与子代理 UI 重做。
  - **卡死（恶性 bug）真因**：`TerminalManager::spawn` 在**持有终端全局锁 mu_** 的情况下执行前台等待
    （`reproc_wait`）。两个会话同时跑 `terminal_exec sleep 60` 时，第一个 sleep 把 mu_ 拿走 60 秒；第二个
    spawn 的回合线程、每 60ms 一跑的终端 poller（同时负责 `on_timer_tick` 唤醒调度）、UI 的 terminal.grid
    请求全部排队。两个回合 + 余额 + 网格把 4 个 NativeBackend worker 占满后，plan/唤醒/状态请求都只进不出，
    界面看起来完全卡死；后端自己还在跑，所以关掉再开能看到回合已经完成。这就是"plan 不更新、也没能计划唤醒"。
    现在前台等待**释放 mu_**（只把注册/收尾留在锁内），`fg_wait` 标记让 tick 继续收输出但把退出判定留给
    waiter；worker 上限 4→8；SlowEventLog 的开关以前写反了（默认关），现在默认开，慢 render 也写
    `%TEMP%\flashsight_res.log`。新增 `terminal_foreground_wait_does_not_block_the_manager`（前台 4 秒命令
    期间 tick/另一个 spawn 必须 1.5 秒内返回）。
  - **wakeOnExit 是死的**：`AgentHost::on_terminal_exit` 没有任何调用者，`wake.fired` 也没有消费者——
    后台终端退出后不会续跑，模型也没有任何工具能"计划唤醒"。现在：
    - `terminal_tick` 在 justExited 时按 TermEntry.wake_on_exit 调用宿主；`wake.fired` 会真正启动一轮
      （专用可 join 的唤醒线程；任务已在跑则转成 round-end 引导，不并发开第二轮）。
    - 新增模型工具 `wake_schedule` / `wake_cancel` / `wake_list`：安排 delayMs 后自动续跑，**不要再用
      terminal_exec sleep 等待**（那会占一整个回合）。唤醒提示是 `SYSTEM: ...`，要求先看 plan/todo 再继续。
    - WakeScheduler 加锁（schedule 来自工具线程、due 来自 poller）；fire 在锁外执行，避免回调里再调度死锁。
    - UI 收到 wake.scheduled/fired/cancelled 立即刷新唤醒条与 loop 状态。
  - **模型能力印象更新（参考 temp/models.md）**：`default_context_for` 改成按 2026 家族表取值，再回退到
    关键词：DeepSeek V4 Pro/Flash 1M、GLM-5.3 1M / GLM-5 200K、Kimi K3 1M（K3-256k 256K）、MiniMax M3 1M /
    M2 204.8K、MiMo V2.6 1M / V2 256K、Qwen 3.6-3.8/Max 1M、Grok 4.7 500K / 4 Fast 128K、GPT-5 400K /
    GPT-5.6/6 1.05M、Claude 5.x 1M / Claude 4 与 3 200K、Gemini 3.5 Pro 2M。只有**未知家族**才用
    mini/nano/flash/lite/size 这类 token 降档；DeepSeek/Qwen/GLM 的 Flash 不再被误判成小窗。
    原生窗口实测：deepseek-v4.1-flash / glm-5.3-flash 1000K、grok-4.7 500K、muse 1048K、agnes-*-flash 131K。
  - **思考档位首次进入**：首帧先用和后台相同的 `thinking_levels_for()/default_thinking_for()` 兜底，不再先
    渲染关/开对；上一批的"选模型立即刷新 + 非法旧档位清掉"保留。新增 deepseek-v4 / qwen3.6-3.8 / glm-5 /
    kimi-k3 / minimax-m3 / grok-4 / gpt-6 等档位识别。
  - **上下文过限**：核查确认 `HistoryBuilder` 在**上一次请求的 prompt tokens ≥ 窗口 80%**（可设置）时压缩：
    保留最新 `keepTurns`（默认 8）轮，其余折叠成摘要 user 消息（带 priorSummary），发 `context.compressed`
    提示。不是清空重来；单轮就超限时压缩跳过硬上，由 provider 报错。
  - **子代理 = 主会话的隔离工作区**：`complete-step` 现在**强制阻塞**并直接返回摘要（以前 wait 默认 false，
    模型拿到 running 就继续，于是干脆自己把活干完，主上下文仍然被工具输出占满）。提示词改为"规划完就必须
    派活"：清单建好后逐项 spawn complete-step，主会话只保留规划与摘要；子代理工作记录留在自己的会话/侧栏。
    实测主上下文只收到 `{summary}`，完整 transcript 不进主历史。
  - **子代理 UI**：总览卡片任务/结果截断（代码 + 3 行 CSS clamp），不再一条占满侧栏；完成卡片始终显示
    "结果摘要"；运行中卡片有高亮边框 + "运行中"徽标 + 已运行时长；详情页顺序改为
    任务 → 引导 → 过程记录（明确标注按时间顺序、最新在最后）→ 结果摘要（已回传主会话）；运行中详情实时
    追加子代理文本。主输入框在 complete-step 运行时显示"当前步骤在子代理中执行 / 这里的输入发给主会话；
    要引导它请打开记录"，避免引导发错对象。
  - **测试**：437 用例 4085 断言全过；web / webview 双变体编译通过。原生 WebView2 实测首帧思考档、更新后的
    上下文数值、子代理面板均正常。


<114>(FIXED/AUDIT): 自动压缩默认关闭（不再砍旧轮/砸缓存）+ 子代理思考与主回答彻底分流 + 事件分发的会话归属修复 + 删掉重复的唤醒实现。
  - **自动压缩 = 赔本买卖**：后端把 `history.compressAtPct` 默认写成 80，等于用户什么都没设置也在水位到 80% 时
    悄悄把旧轮折叠掉。压缩会改写提示前缀，provider 的前缀缓存全部失效（通常更贵），而且旧轮不再原样进入上下文，
    会话完整性被破坏。现在默认 **0（关闭）**：`HistoryBuilder::Options`、宿主三处读设置、上下文状态、设置页默认
    全部改 0；设置页显示"关闭（推荐）"，并明确写出"完整保留上文与缓存；开启后会改写提示前缀，通常更贵且丢失旧轮"。
    手动/显式阈值仍可用，但必须是用户选择。新增 `compress_defaults_to_off`（99% 水位也不压，所有轮仍在）。
  - **"思考又留在外面"的真因**：UI 的 `apply_session_event` 对 `fromSubagent` 事件只提前处理了 sidebar 的
    live_text，然后让 `complete-step` **继续落入主会话处理器**。于是子代理的 stream.delta 被追加进主回答的
    `msg.text`（正常颜色、正文位置），它的 tool.start 变成主工具行，连它自己的 turn.done 都可能改写主回合状态。
    这就是"修一个坏一个"的来源：一个 agent 的传输事件驱动了另一个 agent 的状态。
    现在所有子代理事件在更新侧栏后**无条件 return**；complete-step 的实时文本只进 `ChatMsg::work_live`，
    在工作过程里用 `.work-note.work-subagent`（更弱的 non-emphasis 色）显示，子代理 done 时清空（完整记录仍在
    子代理面板）。主回答 `text` 只由主模型自己的 stream.delta 写入。新增断言：子代理流文本不能出现在主回答里。
  - **事件数据源/会话归属审计**：
    - `auto.next` / `auto.halted` / `todo.updated` 以前只在活动标签的分支里处理；`todo.updated` 甚至有一段永远
      走不到的 background 分支（因为更外层的 early-return 已返回）。现在它们统一走 `apply_session_event`，
      按 `is_active` 写入 `AppState` 或 `SessionView`；`SessionView` 补了 `auto_round` 并随切标签保存/恢复。
    - 后台会话的 `loop.interrupted` / `loop.paused` 不会清运行态，切过去还显示"运行中"；现在后台 view 也会清
      `turn_running/auto_round` 并记录中断原因。
    - `turn.done` 的后台分支错误地清了**活动会话**的 `st_.auto_round`，现在清 `vp->auto_round`。
    - 事件总线的 session-scoped 列表补上 auto/todo，活动与后台走同一条路，减少"这个事件只在某个页面生效"的特判。
  - **后端重复功能**：`AgentLoop` 里还留着一套从未被置位的 `wake_scheduled_/wake_at_ms_`、`cancel_wake`、
    `on_terminal_exit`、`on_timer`（WakeScheduler 已取代它）。全部删除，`AgentHost::cancel_wake` 只操作唯一的
    WakeScheduler 并发一次事件。
  - **仍存在的架构债（本轮记录，不假装修完）**：webview 适配器胶水里仍硬编码了 `.tab/.ptab/.tab-close`、
    `.model-field`、`.splitter-handle`、`.title-btn` 等业务 CSS 选择器。这些应改为框架级 `data-fs-*` 约定
    （拖拽条/弹出锚点/分割条），让 wasm/webview 两个适配器都不认识业务类名；这是下一轮独立重构，不在本轮
    为赶进度硬塞。
  - **测试**：438 用例 4093 断言全过；web / webview 双变体编译通过。原生窗口冒烟：设置/会话正常渲染。


<115>(FIXED): 阈值与短视图彻底解耦；并找到上一轮“默认关闭”实际没生效的原因（SettingsStore 里还写着 80）。
  - **阈值 ≠ 短视图**：`history.compressAtPct` 只是“某次调用完发现窗口到了 X%”的**触发信号**；达到阈值后
    怎么处理是另一个独立选择。新增 `history.compactAction`：默认 `"warn"` —— 只发
    `context.compactSuggested` 提示，**历史原样完整发送**；只有显式选 `"shortView"` 才折叠旧轮。
    `force_compress`（手动压缩）仍然折叠，因为那是用户主动点的。设置页现在是两个独立控件：
    “上下文水位阈值”（默认关闭）和“达到阈值时”（默认仅提醒）。
  - **上一轮为什么“默认关闭”没生效**：`SettingsStore::default_value` 里 `history.compressAtPct` 的默认值
    仍是 80。宿主和 UI 读取时虽然写了 0，但设置层总会返回 80，所以默认行为其实还是自动压缩。这正是
    “同一事实多处默认值”的重复来源。现在 SettingsStore 默认 0，并新增 `history.compactAction = "warn"`；
    `settings_new_keys_defaults` 用例同步断言。阈值设置不再隐含短视图。
  - **提示**：到水位时 UI toast “上下文已到 X%：历史仍完整保留；可在设置里选择是否压缩”，不再静默折叠。
  - **测试**：440 用例 4109 断言全过。新增 `water_level_warns_without_short_view`（阈值 95% + keepTurns=1
    仍然一轮不丢）和 `loop_warns_at_water_level_without_dropping_history`（发提示事件、请求里仍有最早一轮）；
    原水位折叠用例显式选择 `shortView`。web / webview 双变体编译通过。


<116>(FIXED): 压缩行为按原意重做：只保留水位阈值；到达阈值全量压缩；只有当前窗口装不下才自动短视图并提醒。
  - **删掉多余设置**：`history.compactAction` 和“达到阈值时”整项、以及“只是触发信号/默认关闭”的说明全部删除。
    设置页只剩"上下文水位阈值"一个控件（默认 80%），不新增任何开关。
  - **阈值 = 全量压缩**：达到水位（或手动压缩）时，`build_neutral` 把所有已存轮次折进摘要（`keep=0`，
    `mode="full"`、`droppedTurns=总轮数`），不在阈值路径保留最近几轮原文。之前把阈值和“只留最近轮”绑在
    一起是错的。
  - **窗口不够 = 自动短视图 + 提醒**：如果 `contextLimit` 小于 `max(全量历史估算, 上一次实际 prompt tokens)`
    （典型场景：从 1M 模型切到小窗模型），全量压缩会丢掉刚做的工作上下文，这时才自动保留最近 `keepTurns`
    轮、折叠其余（`mode="short-view"`），事件里带 `keptTurns`/`droppedTurns`/`reason=window-too-small`，
    UI toast 明确提示“上下文窗口不足，已自动改用短视图（保留最近 N 轮）”。正常到阈值不会走短视图。
  - **默认值回到 80**：`HistoryBuilder::Options`、AgentHost 三处读设置、SettingsStore 默认、UI 默认统一为 80，
    避免上一轮“设置层还写着 80/0”那种多处默认值不一致。
  - **测试**：440 用例 4112 断言全过。新增/改写：`compress_defaults_to_full_at_water_level`（默认 80、全量、
    消息只剩 system+摘要）、`compress_water_level_folds_every_turn`、`window_too_small_uses_short_view`、
    `loop_short_views_when_window_too_small`（短视图事件 + 最近轮原文仍在请求里）；设置页测试只允许阈值一个控件。
    web / webview 双变体编译通过。


<117>(FIXED): 全量压缩不能只折 keyPoints——补上逐轮摘要，避免压缩后只剩一句“已压缩”。
  - 复核 `make_fold` 时发现：它只收集 turn 里可选的 `keyPoints`，而 keyPoints 只有在模型主动调用
    keypoint 工具时才存在。没有 keyPoints 的会话一旦压缩，摘要就只有一行 "[Earlier conversation was
    compacted]"，等于把全部上下文抹掉。全量压缩必须先真的“压出内容”。
  - 现在 `make_fold` 在 keyPoints 之外总是追加 `[Turn digest]`：每条 user 轮取 promptRaw、assistant 轮取
    streamSummary（截断到 260 字节、压成一行），assistant 轮附工具名；`forgotten` 轮仍然完全跳过，不进入
    摘要。摘要总字节有上限（全量 32K，短视图 6K），避免摘要自己撑爆小窗。
  - 测试加强：全量压缩的摘要必须包含真实对话（`A: answer 5` / `U: question 5`），短视图摘要必须包含被
    折叠的旧轮内容；`history_forget_and_keypoints` 继续验证 forgotten 轮不出现。
  - **测试**：440 用例 4115 断言全过；web / webview 双变体编译通过。


<118>(FIXED/AUDIT): 框架/适配器这一层的弹层焦点、剪贴板、页面切换状态与实时快照统一修整；不是再单独打症状补丁。
  - **模型选择器盖住输入框（点两次/先失焦）**：选择器是按“模型按钮”定位的，而按钮在输入框下面，于是
    980x590 的弹层向上展开时**盖住了 textarea**。用户点的“外面”其实点在模型行上：第一次只是选中/关闭，
    输入框根本没拿到焦点，看起来要点两次。现在 composer 的选择器以**输入框**为锚点向上展开，弹层底部
    停在输入框上方；`placePopup` 同时给弹层加 `max-height`（上方/下方剩余空间），永远不再覆盖锚点。
    另外删除了 `picker-backdrop`（它吞掉第一次点击），外部关闭统一交给 outside dispatcher。
  - **外部点击关闭的时序**：mousedown 立即 render 会拆掉目标节点，mouseup/click 落到 `<body>`（焦点丢失）。
    两个适配器都改成**收集要关闭的弹层 → setTimeout(0) 后再关闭**；关闭所有不含目标的外层弹层，而不是
    遇到第一个就 return。C++ 的 outside 回调 id 按弹层节点**稳定复用**（原来每帧换新 id，延迟关闭时会
    找不到回调）；wasm 也从“每个弹层一个 document 监听器”改成统一 dispatcher（不再泄漏节点闭包）。
  - **右键粘贴**：WebView2 会拒绝脚本发起的 clipboard 读取（经 C++ 往返后用户手势已丢失），
    且点菜单项后 `document.activeElement` 已不是输入框。现在：
    - contextmenu 捕获阶段记录 `ctxTarget`，编辑命令一律用它（不是 activeElement）；
    - 原生适配器新增 `read_clipboard_text()`（Win32 `CF_UNICODETEXT`）和 `insert_text_at_caret()`，
      粘贴走“C++ 读系统剪贴板 → JS 插入光标处并触发 input”，完全不需要浏览器剪贴板权限；
    - 浏览器构建保留 navigator.clipboard 回退。实测右键→粘贴后 textarea 值为 `PASTE_FINAL`。
  - **同名模型不同供应商看不出变化**：模型按钮在窄宽度下把 provider 标签 `display:none`，切换后文字完全
    一样；现在 provider 始终可见（紧凑模式只缩小），按钮 title 也带 provider。实测 Happy/A6 两个
    `deepseek-v4.1-flash` 单击一次即可切换，按钮显示 `Happy API | …` / `A6API | …`。
  - **切页面状态/错误气泡**：
    - 切换标签会清理**完成菜单**（complete_kind/items/query）和 picker 的 owner/models/loaded；以前它们
      会带到下一个会话，`data-keys-capture` 还留在新 textarea 上。
    - `model_available` 是后端检查结果，不再从 stashed view 恢复 false；打开会话先乐观可用，检查失败也
      不再把发送按钮永久卡死。
    - `call_async` 增加 quiet 模式：模型可用性、usage、todos、loopState、balances 这些**被动刷新**失败
      不再弹全局错误气泡（面板自己显示空状态），用户动作才提示。
    - 选择模型的 setModel→setProvider→refreshConfig 现在**串行**：之前 refreshConfig 可能先于
      setProvider 执行，读回旧 provider，和按钮显示不一致。
    - 子代理 done 事件补 `sid`：以前 background 会话的子代理完成会被当成活动会话（空 sid 视为 active），
      可能清错 transcript 的 work_live。
  - **切页面思考跑到输出**：`live_state()` 的 text 以前累积**所有轮**的流式文本（包括已完成轮的过程解说），
    重新打开运行中的标签就把全部过程当回答。现在每轮结束把该轮解说移入 `live_interim_` 并清空
    `live_text_`；快照带 `interim/interimAfter`，UI 的 attach_live_turn 把它还原到 ChatMsg::interim
    （工作过程，非强调色），text 只剩当前轮回答。
  - **空模型会话的发送/Enter**：以前只 toast 后无动作，看起来像按钮/回车坏了。现在发送时如果没有可用模型，
    会提示并**自动打开模型选择器**（输入保留）。实测空模型会话按 Enter：toast“请先选择模型”+ picker 打开。
  - **测试**：441 用例 4126 断言全过；新增 live snapshot 的 interim 分离断言、切标签清理完成菜单/picker 状态。
    web / webview 双变体编译通过。原生窗口实测：弹层不再盖住输入框、单击外部关闭且焦点留在 textarea、
    单击切换同名校验的供应商、右键粘贴、空模型 Enter 打开选择器。


<119>(FIXED/AUDIT): 供应商路由的活循环同步 + WebView 批量写入的 UTF-8 根因（卡死/模板页） + edit_file 纯区间替换 + 历史精简/缓存 + 切页气泡去重 + 终端显示编号。
  - **A6 会话实际走 Happy（auto 轮仍用旧 provider）**：`session.setModel`/`setProvider` 只写了 sessionSettings，没有更新已经存在的 AgentLoop。用户中途把模型从 Happy 换到 A6 后，正在自动轮的这个 loop 仍用旧 provider；下一条请求去了 Happy（A6 日志为空）。现在新增 `session_set_model_provider(sid, model, provider)`，选择器只发一个 `session.setModel`（带 providerId），后端在同一个锁里写两项并立即 `AgentHost::set_session_model()`；单独的 setModel/setProvider 也会同步活 loop。Provider 标签在紧凑模式也不再隐藏（同名模型切换后按钮必须能看出换了供应商）。
  - **11:00 卡死的现场**：`%TEMP%\flashsight_res.log` 在冻结前出现 pump_gap 186/257ms，随后 10:59:16 后没有 pump；同时该会话 144 条消息、933 个工具块。usage 记录显示 11:01:44 后端仍完成了一次 135s、402k/401k cache 的调用——即 UI 线程卡住、后台继续。根因在 WebView DOM 桥：每帧几万个 ExecuteScript 调用把 WebView2 队列压垮；而且只要**任意一个 op 的字符串含非法 UTF-8**，`widen_string` 会让整批脚本静默不执行。
  - **批量写入的正确实现**：`enqueue` 改成 JSON 元组数组，一次 flush 最多 256KB，由 `__fsDomBatch` 顺序执行且逐 op try/catch；flush 前做 `utf8::valid/sanitize` 兜底。之前“批量丢尾”的真实触发点是供应商模板页的首字母标记：`t.name.substr(0,1)` 把中文名截成单字节 0xE8，整批脚本非法 → 模板页从 26 张变成 0 张。现在取首个 **UTF-8 字符**，并验证 26 张卡恢复。render 的 do/while 也加了 16 次上限，防反馈环把 UI 线程永久占住。
  - **切页 User-User-Assistant-Assistant(running)**：`load_session` 的 keep 逻辑保留了本地无 turn_id 的 streaming assistant，attach_live_turn 又追加一条；自动轮的用户轮在磁盘里渲染成 system 分隔条，live snapshot 的 "SYSTEM: continue" 又被追加成 user。现在 load 时丢弃本地 live assistant（运行/崩溃由 live snapshot 重建，已结束的回合在磁盘上），attach 时若末尾是自动轮 system 分隔条则不再追加 user；live snapshot 的 text 只含当前轮，完成轮的解说进 `live_interim_`（切页后仍在工作过程、非强调色）。
  - **edit_file 改成纯区间替换**：schema/描述去掉 `expect`，`edits:[{start,end,newText}]`，只校验文件 hash，不再因为“引用的旧文本不一致”拒绝；`start` 缺省时 end 视为单行。用户明确说过“区间替换而不是文本替换”，以前的 `expected text does not match lines 25-25` 就是文本校验造成的。
  - **历史重放精简（full.json 仍完整）**：
    - 工具 **参数** 以前原样重放：write_file 的 content、edit_file 的 newText/expect、plan_update 的整篇文档每轮都再发一遍。新增 `project_tool_args`：write_file 只留 path/bytes，edit 只留 path+行区间+newBytes，plan_update 只留 status，terminal 命令截断，其他有界。
    - 工具 **结果** 去掉了 `truncated`/`detail` 这类重放噪声，目录名用逗号紧凑连接；read_file 样本降到 400 字节；plan 结果只留 status/path/exists。
    - 结果 JSON 用紧凑 dump（无缩进空格）。新增用例断言 8000 字符的 write_file content / 4000 字符 edit newText 不会进入历史。
  - **缓存 52% 的中间可变内容是 git**：`build_system` 的环境块里带着 branch/dirty/ahead/behind；模型一编辑/提交，system 前缀就变，provider 的 prefix cache 从这里全部失效。git 状态移到 **runtime 尾块**（追加在最新 user 消息），system 字节稳定；新增用例比较 dirty 前后 system 完全相等、runtime 含 git。
  - **终端编号**：短前台命令即使记录被裁剪也消耗 tid，列表会显示 #107。UI 现在显示终端在用户列表中的序号（1..N），不再显示后端 tid；新增用例。
  - **语义确认**：todo 是列表（未清空自动轮继续），plan 是文档（未 done 自动轮继续，用于产出 todo），workflow 是并行大流水线；WhaleUI 那次 plan_update done 但 todo 还有未完成项，所以自动轮继续是符合规则的，不是把 plan 当成 todo。
  - **测试**：445 用例 4150 断言全过；web / webview 双变体编译通过。原生窗口实测：供应商模板 26 张、选择器单击切换/外部关闭焦点保留、右键粘贴、空模型 Enter 打开选择器。另：清理了此前验证时误写进 new1 会话 111 的两条测试消息。


<120>(FIXED/AUDIT): 切页实时工具/气泡、会话排序与长列表、终端标签、edit_file 读回/撤销/行号、自动轮上限、todo 回收；CI 变体改为布尔开关。
  - **切页后工具调用消失**：运行中的工具行只存在 loop 内存里，切页重建 transcript 时只从磁盘读，于是只剩思考。`live_state()` 现在带 `tools`（每完成一次工具就更新、上限 200），UI `attach_live_turn` 用它重建 ToolBlock（name/summary/detail/args/subagentId），不再依赖本地临时气泡。
  - **SYSTEM: 原文与 User-User**：自动轮的 userText 是 "SYSTEM: continue"；如果末尾没有 system 分隔条，现在创建“自动继续 · 第 N 轮”标记，绝不把它渲染成用户气泡；分隔条存在时不重复追加。
  - **会话排序**：`session_list` 按 `updatedAt` 降序、sid 次序稳定排序，更新的会话回到项目列表顶部。
  - **>10 会话**：项目卡片新增独立的 `sessions_open`，默认只显示 10 行并给“展开/收起”；它和窄屏的整块展开分开，长项目不会再挤掉其他项目。原生窗口实测 17 个会话的 new1 默认 10 行、展开后 17 行。
  - **左键拖动**：wasm/webview 的分隔条与标签拖动只接受 `button === 0`；WhaleUI 的 `bind_drag`/`bind_nav_drag` 加 `whaleui_dom_event_mouse_button(ev) != 0` 拦截。原生窗口实测右键拖动宽度 240→240，左键 240→300。
  - **终端标签**：`terminal.list` 里 model-fg/model-bg 只进“所有终端”，不再自动成为标签；`load_terminals` 只把 user 终端清出 `term_hidden`，并通过 `terminal.output` 先出现的终端先隐藏；tab strip 真正按 `term_hidden` 过滤（之前只收集没消费）；`terminal.exit` 重新拉列表，短记录被裁剪后标签/计数同步；当前标签被隐藏时回退“所有终端”。原生窗口实测修复后只剩“所有终端/新增”，从概览点开才出现标签。另把此前测试写进 whaleui 会话的 6 条测试终端记录（150/257/269/276/335/365）连同日志移到 `temp/whaleui_test_terminals_backup/`，不再污染列表。
  - **终端返回重复**：`terminal_exec` 不再自己 poll，foreground spawn 已返回最终输出，结果里不再出现 `output` + `poll.output` 两份；collapsed 行 summary 是命令前 80 字节。
  - **edit_file 协议重做**：
    - 只接受 `{path,hash,edits:[{start,end,newText}],lineEnding}`；未知字段（count/ranges/expect…）直接报错，不再静默通过；start/end 必须同时出现且有 1-based 整数；hash 必填且校验。
    - 写入后读回文件，按每段实际落盘内容返回 `before/after`（不是只回显 newText）；行数不符或读回文本与 newText 不同会进 `warnings`。`newText` 的括号/花括号不平衡、块注释未闭合也会警告。
    - 返回 `totalLinesBefore/totalLines/hash/beforeHash`；summary 明确提醒“改动以下的编号已移位，下一次编辑前重新读”。
    - read_file 默认返回带真实行号的内容（`N\tline`），`numbers=false` 才是原文；summary 是 `path start-end/total lines`，collapsed 行直接显示路径。
    - 新增 `edit_revert`：一步恢复本进程最近一次对该文件的 write/edit，不用退回 git checkout；用完即失效。
    - 历史重放仍只保留路径+区间（before/after/newText 不进后续上下文），同一内容不会每轮重复。
  - **自动轮 12→50**：`Options::max_auto_continues` 默认 50；空轮/计划完成仍会停，重复守卫不变。
  - **todo**：`todo_list`/`todo_check` 能带出已完成项；新增 `todo_erase` 让模型删除单条不需要的任务（不是只能清空）；新增“重启后已完成项还在、下一 id 接续”用例。
  - **CI 变体选择**：`workflow_dispatch` 从 `variant: all|web|webview` 三选一改成 `web`/`webview`/`whaleui` 三个独立布尔开关；tag/release 仍默认编 web+webview，WhaleUI 额外受 `ENABLE_WHALEUI` 控制。
  - **跨平台**：新逻辑都在共享 UI/工具层；WhaleUI 的拖动加左键判断，`copy_text` 走 `whaleui_clipboard_set_text`（不再空实现）；Win32 剪贴板仍只在 `_WIN32` 分支，非 Windows 保留 `navigator.clipboard` 回退。
  - **测试/构建**：448 用例 4186 断言全过（含严格 edit schema、读回编号、revert、长列表、todo 恢复/erase）。web / webview / whaleui 三变体编译通过；原生窗口实测右键拖动无效左键有效、17 会话默认 10 行、终端只剩“所有终端/新增”。live 工具快照路径未在真实模型回合复测（当时没有可跑的回合），由代码路径与快照用例覆盖。
  - **闪退**：事件日志里没有 FlashSight 的 Application Error；同一时间只有 0x10e GPU bugcheck / BlueScreen 记录，应用侧无崩溃转储，无法本地复现。已保留/加强 WebView 批量与 UTF-8 防护，但没有证据把那次退出归到具体代码路径。


<121>(FIXED/AUDIT): 压缩结果持久化并在重开时复用（修复 16% 却超窗→9% 短视图和缓存重排）、read_file 行号改用 U+2502、自动轮取消硬上限（迭代续跑）、0x10e 崩溃溯源。
  - **重开丢压缩内容**：压缩摘要以前只存在于当轮 `build_neutral` 的内存里，从不落盘。重开会把完整历史重新发一遍，把已经被摘要覆盖的旧轮再算进窗口，于是“打开显示 16%，发送却说超上下文，自动短视图后变 9%”，早期信息被丢。新增会话级 `compaction.json`（`{summary, compactedTurns, lastTurnId}`）：
    - `AgentLoop` 每轮开始前加载；全量压缩后保存，没有全量摘要时短视图也保存（见下）。重开/下一轮直接复用摘要。
    - `build_neutral` 只回放 `summary + turns[compactedTurns..]`，窗口判断用**有效历史**（摘要 + 未折叠轮次），不再用完整 transcript；旧轮不会重复计数。
    - `lastTurnId` 校验 + 折叠区间内 `forgotten` 检查：fork/rollback/forget 后不会把旧摘要套到不同内容上。
    - 短视图在**没有全量摘要**时也落盘（否则已存在的长会话每次重开都会重新生成同一个短视图）；但它不会覆盖全量摘要，且记录 `mode=short-view`，换回大窗口时会被忽略并恢复完整历史。摘要头不会每次压缩都嵌套一层。
    - 复用摘要后 prompt 前缀是 system + summary + 固定边界后的轮次，重开不再重排旧上下文；provider 缓存命中是否恢复需真实回合验证（本次没有可跑的模型回合）。
  - **read_file 行号歧义**：`N\tline` 遇到 tab 缩进文件会变成 `2\t\tcode`，模型无法分辨哪个 tab 是行号。改成 `N│line`（U+2502），返回 `lineFormat`，描述明确“第一个 │ 前是行号，│ 后是原样内容（tab 保留），不要把 N│ 写进 newText”。`numbers=false` 仍是原文。
  - **自动轮硬上限**：`max_auto_continues` 默认改为 0（不设硬上限），停止交给 todo/plan/空轮/重复守卫。直接把 12→无限会让原来的 `return run_loop(next)` 递归无限加深（测试直接 `0xC00000FD` 栈溢出）；现在 `run_loop` 用 trampoline：自动轮把下一轮 messages/model/tools 放进 `auto_pending_*` 并返回，外层循环继续，不递归。测试里会无限产生工具调用的桩显式设 cap。
  - **崩溃溯源**：最新的 `110726-42171-01.dmp` 是 `0x10e`，参数 1 = `0x2D`；按 Microsoft 文档这是 `DdiMapCpuHostAperture` 失败，属于**显卡驱动**不可恢复错误，不是应用异常（Application 日志无 FlashSight Error/WER）。最后一条资源日志 21:23:56：tools 318→359、terms 0→11、handles 478→561（没有失控增长）、pump_gap 53–70ms；21:39:20 系统 bugcheck 后重启。GPU：NVIDIA RTX 4060 Laptop 32.0.15.9174（2025-12-30）/ Intel Iris Xe 32.0.101.7076。建议更新显卡驱动；应用侧无法捕获 kernel bugcheck。资源日志与用户描述的 20:31 有一小时差，以事件日志时间为准。
  - **测试/构建**：454 用例 4223 断言全过（新增压缩持久化/重开不短视图/短视图状态复用/陈旧状态忽略/loop 复用摘要、tab 缩进行号、自动轮 cap 用例）。web / webview / whaleui 三变体编译通过。


<122>(ROOT CAUSE/FIX): 任务完成后的自动轮空转把系统 commit 耗尽；不是“驱动背锅”。
  - **现场数据**：逐轮检查 `s_fdd...` 会话的 12 个 turn shard（182 turns）：**49 条 `SYSTEM: 继续` 用户轮，46 条 assistant 文本为空且 0 工具调用**。任务已经被认为完成，但 open todo 让空轮守卫豁免，自动轮继续反复唤醒/请求/重渲染，直到旧 cap。这正是“每轮结束后迅速卡死”的急性路径。
  - **系统证据**：21:33:10 Resource-Exhaustion Detector 1003：commit limit `68,438,171,648`，commit charge `68,419,985,408`（99.97%）；21:33:16 1007：申请 `582,000` 字节失败，`0x8007000E`（ERROR_NOT_ENOUGH_MEMORY）；21:38:05 再次 1003。系统是**commit 耗尽**，不是单纯驱动故障。`0x10e / 0x2D DdiMapCpuHostAperture` 是后续：内存无法映射/提交时 GPU 驱动报错，所以之前“只说更新驱动”是错的。
  - **自动轮修复**：新增 `empty_auto_streak_`。连续 2 个 auto round 没有任何工具调用就停止，即使 todo 仍未清空；工具轮清零，用户手动发起的新轮也清零。保留无硬 cap + trampoline；旧的“open todo 无限续跑”不再成立。测试把 todo 空轮用例的 cap 改成 10，仍应只剩 2 个 auto.next。
  - **真实内存增长路径**：WebView / WhaleUI / Wasm 三个适配器的 node slot 表原先都是按单调 node id 索引的 vector：节点删除只把值置空，vector 不缩，永远保留“曾经创建过”的每个节点。JS 侧 `__fsNodes`/`__fsHandlers` 也是数组置 null，同样只增不减。改成 map/object + erase/delete，只保留存活节点。长会话/反复自动轮渲染会累积这条路径。
  - **资源记录升级（这次有测量）**：两个 native host 每 30 秒写 `[res]`，字段包括 `mem_ws/peak/private/commit`、`sys_avail/commit/limit`、WebView2 子进程数+commit 总和、`threads`、`handles`、`gdi/user`、render 线程 `stack_peak_kb`、消息/工具/终端字节数、`dom_slots/live/made/handlers`、`auto_round/turn_running`。之前只有函数耗时和 handles，无法验证内存理论；以后崩溃后有 commit/线程/自动轮状态。
  - **基线实测**：原生窗口打开 30 条消息/349 工具块：main private 12→20MB、WebView2 组 1.30→1.40GB、system commit 14.5GB/41.7GB、threads 15、dom_slots 2655（nodes_made 2676）。这是正常基线；下次事件按日志中的 commit/threads/auto_round 判断是否复发。
  - **测试/构建**：454 用例 4223 断言全过；web / webview / whaleui 三变体编译通过。


<123>(ROOT CAUSE/FIX): 发送即卡死 = Markdown 渲染器在流式首字符 `#` 上的死循环，不是内存/GPU。
  - **复现**：从 23:43 的 live snapshot 取出发送中的回答文本，对每个前缀跑 `comp::markdown_blocks`；**前缀长度 1（单个 `#`）就 100% CPU 永不返回**。之前的测试进程就是这样挂着被手动杀掉的。
  - **原因**：标题分支要求 `#` 后面有空格（`n < ln.size() && ln[n]==' '`）。模型流式输出 `#`、`##` 时还没有空格，标题分支不消费；随后段落收集在 `l2[0]=='#'` 时 `break`，但也不 `++i`，`while(i < lines.size())` 永远原地打转。每轮循环还会 `out.push_back(<p>)`，所以既是 100% CPU，也是**无界 VNode 分配**；发送后第一个 delta 就是 `#`，UI 线程立刻开始吃内存，画面/时间/转圈全部冻结。**0:04 那次已经查清：不是 WhaleUI，是我们 23:59 启动的 fs_tests 诊断进程卡在同一个前缀循环里；[res] 显示 app 主进程仅 21MB、WebView2 组 1.37GB 稳定，而 system commit 从 47GB 涨到 65GB、avail 到 0，正是它在每轮分配 `<p>`。已在定位后杀掉，系统随后恢复；这部分是我的测试干扰，抱歉。**
  - **修复**：新增 `is_heading_line()`：`#`..`######` + 行尾/空格都算标题；`#`/`##` 直接消费成空标题，段落收集只在**合法标题**处 break。`#foo` 这种不是标题的行走普通段落。回归用例 `comp_markdown_incomplete_heading_terminates` 覆盖 `#`/`##`/`#foo`/`## ` 以及一条真实流式回答的所有前缀（修复前该用例会挂死）。另加 `max_steps = lines*8+64` 的进度守卫：以后任何分支不漏 `++i` 也只截断，不会无限分配。
  - **md4c 真实情况（回答用户提问）**：仓库里确实 vendored/编译了 `md4c-master`（`md4c.c`/`md4c-html.c`/`entity.c` 在 fs_core 里），但 **src 里没有任何 `md_parse`/`md_html` 调用**；实际 UI 用的是 `ui_components.cpp` 手写的 `markdown_blocks()` + `inline_md()`。所以不存在两套解析器，只有一套手写解析器，md4c 目前是死权重。正常进入能渲染二级标题，是因为完整的 `## 标题`（有空格）走对了手写标题分支；只有流式半截 `#`/`##` 触发死循环。之前要求的“魔改 md4c 输出 diff AST”从未接入实现；这是一笔明确的债，不能把 md4c 的存在当成已实现。
  - **这次不是内存**：23:43:17 的 `[res]` 是 main private 44MB、WebView2 组 1.39GB、system commit 13.7/41.7GB、threads 20；没有 commit 压力。0x10e 崩溃和自动轮空转是另一条线（<122>）。内存日志保留，但本次根因是纯 CPU 死循环。
  - **顺手**：WebView2 子进程/线程枚举移出 UI 线程，改成后台线程每 10 秒刷新原子缓存；避免新加的内存日志本身成为潜在阻塞点。
  - **测试/构建**：455 用例 4285 断言全过；web / webview / whaleui 三变体编译通过。
