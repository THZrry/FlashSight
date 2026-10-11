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
