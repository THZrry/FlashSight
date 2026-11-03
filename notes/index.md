# FlashSight Notes

The process of project: P0 环境搭建

<1>(SOLVED): 网络不可直连 GitHub，本机 v2ray 代理 127.0.0.1:10809（系统代理已启用）。
  所有 curl/git clone 需 `-x http://127.0.0.1:10809`。curl 直连 exit 6/28，走代理后正常。
  经验：环境里 Shell 是 cmd 兼容（无 unix 管道工具），脚本一律 .bat；且每次 Bash 调用是全新进程，
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


## 当前状态

- P0 骨架完成：git 仓库、xmake targets（fs_core/fs_ui/flashsight/fs_tests）构建通过，
  flashsight.exe 与 fs_tests.exe 可运行。
- 3rdparty 六件套（ghsl/json/httplib/mbedtls-3.6.4/md4c/dtl）全部编译或构建验证通过。
- 下一阶段：P1 AppCore（见 roadmap/10-appcore.md），按 build.md §4 先写单测。
## 当前状态（更新 2026-10-12）

- P0-P4 后端+UI 原型+Widget 库全部完成，54 用例 390 断言基线通过。
- 下一步：addon2 后端增强（工具补全/健壮性/长任务范式），不动前端。

<15>(TIPS): 用户 2026-10-12 指示（addon2 实现约束）：
  - list_dir 分基础/详细两种模式：基础只给文件名（默认，省 token），详细才带日期/大小/CSV。
  - regex 从 C:/aaa/WhaleJS 的 qjs 实现移植：libregexp.c + libunicode.c + unicode_gen.c
    + cutils.h/libunicode.h/libunicode-table.h/unicode_gen_def.h；三个回调
    lre_check_stack_overflow/lre_check_timeout/lre_realloc 由宿主提供（quickjs.c 有参考）。
    移植需标注来源（MIT，Fabrice Bellard）。
  - 工具 skill 描述必须简明清晰、中英双语，用设置键切换思考语言。

<16>(PLANNED): addon2 后端增强实现计划（分四阶段，纯后端）：
  A. 工具补全：A1 工具真执行（Loop 批准后真调+结果回填+循环到无 tool_calls）；
     A2 check_dir（子代理筛 ls）；A3 file_regex（qjs 移植）；A4 rm/写接权限体系；
     A5 噪声检测（write/diffWrite 前，unicode 异区+引号不平衡）；A6 list_dir 基础/详细。
  B. 健壮性：B1 中断-继续（保存 turn 上下文+session.continue）；B2 进程 todo 工具
     （含子代理独立 todo）；B3 计划评价 revise 流（feedback 注入下一轮）。
  C. 工作流：C1 WorkflowStore；C2 WorkflowRunner+图数据；C3 步骤重试/跳过/定向引导。
  D. 环境：D1 lsp 工具；D2 浏览器 skill 加载口。
  顺序 A->B->C->D，每阶段补单测 test_addon2.cpp，全量回归保 390+。

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
  - 重做：顶部标签栏单一导航模式（去侧边栏杂糅）；桌面原生风格（直角/1px 边框/标准控件）；
    prefers-color-scheme 深浅色。用户明确讨厌 Material Design（药丸/大圆角）。
  - 构建流程 scripts/build_ui.bat（无硬编码路径，只用 PATH 里的 emcc/xmake/python）：
    wasm 编 fs_ui_wasm -> gen_assets.py 打包 web/ 成 ui_assets_gen.hpp -> windows 编 exe
    （内嵌资产，运行时起 HTTP 服务开浏览器）。WhaleUI 就位后换 UI 后端即可。
  - 关键 bug：BrowserDom next_id_ 从 1 起与 glue.js 保留 root id=1 冲突 -> appendChild 自环
    （HierarchyRequestError 页面全空）。改从 2 起 + glue 自环防护。
  - emcc 6.0.12（C:\Program Files\emscripten）；xmake emcc 工具链默认 -fvisibility=hidden
    会隐藏 extern "C" 入口，需 -fvisibility=default。
  - 已知：响应式框架粗糙，后续大规模重构（用户明确）。

<19>(SOLVED): addon3 —— UI 框架四层重构（DOM Adapter/Framework/Composers/user）。
  完整 IDom + WasmDom(val.h)/NullDom；VNode + keyed diff/patch + EventBus；
  组件库（可关闭标签条/文章式对话/折叠流程块/自由侧栏）；AppState 整体重渲染 + patch。
  关键坑：xmake io.readfile 文本模式毁二进制 wasm（必须 io.open rb/wb）；
  val.h 需 C++17；import()/io 只在 target scope 可见。详见 current.md <19>。

<20>(SOLVED): 构建流程纯 xmake（fs_ui_wasm 作 dep + after_build Lua 打包资源），
  删除 scripts/build_ui.bat 与 gen_assets.py；无硬编码路径；事件端到端已验证。详见 current.md <20>。

<21>(SOLVED): 终端接线 reproc（TerminalManager + 工具回调 + sid 注入），
  fetch_url 接线（NetClient）；vision 改为明确 not-supported（需平台后端）。详见 current.md <21>。

<22>(SOLVED): 纯 xmake 依赖顺序坑 —— xmake 默认跨 target 并行编译，add_deps 只保链接顺序，
  需 set_policy("build.across_targets_in_parallel", false) 串行化；wasm target 需
  set_extension(".js") 才产独立 js/wasm。详见 current.md <22>。

<23>(SOLVED): 用户反馈第一批（设置导航对齐/项目卡片会话区/原生目录选择+拖拽/Enter清空/
  挂起与去switch栏/侧栏拖宽/“+”菜单）+ 两个底层 bug（EventBus::fire 自毁闭包、
  session 解析只靠内存 sid->pid 映射导致重启后丢会话）。详见 current.md <23>。

<24>(SOLVED): 用户反馈第二批 —— 项目卡片双布局（宽屏左信息右会话/窄屏堆叠）、
  侧栏拖拽方向修正、提示自动消失、供应商管理做实（模板/列表/编辑/自定义头/密钥保留/
  x-opencode-session 透传）、MCP 与 Skill 做实（探测/导入/启停/持久化）、
  设置补齐（会话默认/外观字体/网络代理/文件黑白名单/浏览器与电脑控制/快捷键可编辑）。详见 current.md <24>。

<25>(SOLVED): 反馈第二批续 —— 侧栏文件树/操作记录/调用记录接线、key-value 表可增删、
  / 命令选择器；sv_obj 修正设置项读取。详见 current.md <25>。

<26>(SOLVED): 框架层修复 —— 会话状态按标签隔离（SessionView/switch_tab，切回不丢）、
  后台会话事件按 sid 归属、侧栏宽度持久化、挂载动画（页面切换/文字淡入/弹层）、
  skill 选择器改浮层+Esc/遮罩收回；IDom 加 bind_splitter(on_resize)/bind_global_key
  保证两边后端接口一致。详见 current.md <26>。

<27>(SOLVED): 反馈第四批 —— KeyedStore 框架作用域通用化、会话页重排（上栏
  计划浮窗+中历史50%+输入框+可收起侧栏 / 下栏终端）、供应商格式下拉+模板跳表单+高级、
  终端后端非阻塞轮询推事件（去掉手动轮询）、动画分三档（view/组件/文本）。
  详见 current.md <27>。

<36>(SOLVED): 反馈第十三批 —— 资源打包改到 fs_ui 的 before_build（此前 CSS 改动永远
  到不了 exe：after_build 写完头文件时 fs_ui 已判定过新旧）、工具条 flex-wrap:nowrap
  + 发送/挂起 text-wide（高度恒定，不再换行/抽搐）、设置面归位（浏览器/电脑控制移到
  通用、新增模型偏好、子代理模型取自供应商列表、上限改步进、关于页精简）。
  详见 current.md <36>。

<38>(SOLVED): WhaleUI 桌面 UI 接入 —— 头文件取回 3rdparty/whaleui/include/、新增
  WhaleDom（ui_dom_whale.cpp）+ 原生入口（ui_main_whale.cpp）、main.cpp 按
  FS_UI_WHALEUI 二选一、xmake 加 --whaleui 开关（默认关闭，等 WhaleUI 完工）。
  ui_app/vdom/components 一行未改。坑：静态库链接顺序要挂 fs_ui 不是 flashsight；
  WhaleUI 无"改写文本节点"API 故 Node 用槽位下标；布尔属性按存在性；空 style 要
  remove_property。详见 current.md <38> 与 doc-internal/ui-whaleui.md。

<39>(SOLVED): WhaleUI 布局缺陷 —— flex 项主轴尺寸写成视口单位（vh/vw）时塌成
  1px，导致桌面 UI 只画出标签条。根因：layout_flex 走 len_px（视口硬编码 0）
  再被 1px 地板兜住（layout.cpp:3471 / 3515）；百分比基数还用了宽度
  （layout.cpp:3471）。复现 + 报告已交上游 WhaleUI temp/incorrect1/。
  2026-10-17 上游已修（WhaleUI e87979c）：主轴改走 len_or_auto_vp、百分比基数
  按主轴取；复现程序 15 变体全 ok，桌面 UI 完整渲染可交互。详见 current.md <39>。

## 当前状态（更新 2026-10-17）

- P0-P3 + 缺口补齐 + addon2 后端增强 + addon3 UI 框架重做 + 终端/网络接线全部完成。
- UI 四层重构，构建纯 xmake（单条命令从零可构建），事件端到端已验证。
- 终端已是真终端（VT 解析 + ConPTY + scrollback + 搜索）；设置/外观已做实。
- WhaleUI 桌面后端已接入并接线（`--whaleui=y`，默认不编译）；上游修掉 flex 主轴
  视口单位塌 1px（notes <39>）后，桌面 UI 已能完整渲染并交互。
- 等：vision 平台截图/图像解码后端；WhaleUI 缺口 A-G（见 doc-internal/ui-whaleui.md §5）。

<40>(SOLVED): 终端标签与"所有"解耦（关标签不删终端、重开追加到末尾，新增
  `term_order`）+ 空间利用（设置页 grid 双栏、供应商表单双栏 + 模型列表右栏、
  模板网格自适应）+ 输入框去掉重复的「子代理」「终端」按钮 + 侧栏程序化打开
  必弹出 + 依据 rules/goal.md 重排设置（常规只留界面显示/数据保留，新增
  「模型 · 上下文」放短视图/压缩/防过拟合）+ 修 .field .hint 抢行压塌控件。
  161 用例 1560 断言。详见 current.md <40>。

<41>(SOLVED): 模型一等公民 —— 逐模型能力/长度/思考档位/重试（思考档位按模型粗匹配，
  不是固定枚举）、模型列表（启用/测试/删除/新增）+ 高级设置上下分栏、会话模型
  选择器（左供应商/右模型 + 搜索 + >20 首字母分组）、本地图标集（Lobe Icons 63
  品牌 + material-icon-theme 67 文件类型，data URI 生成，含暗色变体）、工具条收缩
  阈值按新内容重测（940→600px）。183 用例 1682 断言。详见 current.md <41>。

<42>(SOLVED): 模型选择器改为锚定输入框的弹出层（原来在 pointer-events:none 的
  overlay 层里，点不掉、切标签也不消失）+ 点文件在侧栏新开"预览"标签页（不再内联）
  + 顶栏/侧栏访问栈（关标签回退到来处，不再固定跳 home）+ 顶层 "+" 打开"新建"页
  （项目卡片列表，不再直接给第一个项目开会话）+ 项目卡片会话行加状态点/相对时间/
  可删除（带确认）+ 左列顶部对齐、按钮图标化 + 修 panel_tabs 缺 key 导致关面板
  触发相邻槽位动作。195 用例 1732 断言。详见 current.md <42>。

<43>(SOLVED): 设置页按 settings1.md 返工 —— 设置/新建改为盖在当前标签页上的页面
  （不是标签、不入栈）、标签位置左/顶真正实现、标签条可滚动（删上限）、删掉「上下文」
  整段、会话默认/模型偏好/网络/终端/环境/MCP/Skill/长期记忆/浏览器控制逐页重做、
  供应商页自动选中+品牌图标+模型全选+能力两列+档位逗号输入、修"获取模型列表"
  结果没入库导致永远显示预置模型、新增 dialog.pickFile 与 CLI 工具探测。
  197 用例 1724 断言。详见 current.md <43>。

<44>(SOLVED): WhaleUI 二次验收 —— 上游修掉 flex 主轴视口单位塌 1px 后，桌面 UI
  完整渲染并可交互；逐屏对比浏览器版又发现 4 个缺陷（flex 项 overflow:hidden 塌
  1px、button 的 height 被 UA min-height 压掉、grid auto-fill minmax 列宽错、
  flex 行含 select 少 52px），复现交上游 WhaleUI temp/incorrect2/。
  详见 current.md <44> 与 doc-internal/ui-whaleui.md §5（H/I/J/K）。

<45>(SOLVED): 上下文水位改成贴在模型按钮底边的细线（删掉整行进度条）+ 自动挡与
  批准模式互换 + 不再预置 GPT 模型（默认模型也不写死，发送时带上当前模型/档位）+
  设置整窗覆盖标签栏 + 用量统计动画（新增真实按天柱状图，柱子从 0 长出来）+
  切页方向感知（回到刚离开的页面播反向动画，关标签恒为后退）。
  201 用例 1822 断言。详见 current.md <45>。

<46>(SOLVED): 框架双 view（离场页面保留 DOM 播退场动画，不再一套元素叠两个动画；
  行/卡片/popover 消失也淡出）+ 左侧栏模式重做为项目导航（常驻全部会话、可拖宽、
  可最小化，与右侧栏各拖各的）+ 标签拖拽排序并持久化 + 项目卡片 hover 不改高度 +
  用量页首屏加载供应商 + 设置文案精简/请求体覆盖上下排 + "记忆上次"可选（独立
  标记 + session.last.*）。204 用例 1873 断言。详见 current.md <46>。

<47>(SOLVED): 设置/新建改成真正的覆盖层（shell 保持挂载，整层淡出后再删；关闭
  也有退场动画）+ 离场方向按本次移动盖章 + 启动即加载设置（左栏/配色/标签顺序）+
  文件权限也支持"记忆上次"。204 用例 1878 断言。详见 current.md <47>。

<48>(SOLVED): 动画错开（离场 80% 后入场才开始；页面入场改原地缩放）+ 新建页不再
  覆盖标签栏（左栏补"新建/主页"入口）+ 文件权限按档位判定（宽松=项目内放行、
  系统操作询问，含 approved 放行与 cwd 接线）+ 补 ~295 条英文并给 200+ 处硬编码
  文案套 tr + 标签拖动浮层/落点高亮。205 用例 1888 断言。详见 current.md <48>。

<49>(SOLVED): 动画再调（离场 .26s/入场延迟 .2s；主页子页改淡入，修"从底部弹起"）+
  Skill 页删无意义文案并把内置 MCP 归到 MCP 页 + 左栏隐藏重复 footer、补新建项目、
  修拖拽抽搐（只写变化的 inline style）+ 侧栏标签拖拽排序 + 会话状态色点（完成/
  报错/待确认/运行中）+ 全局搜索页与左栏筛选排序 + 模型选择器按空间翻转并支持
  点击外部关闭。210 用例 1913 断言。详见 current.md <49>。

<50>(SOLVED): 反向动画与"偶发急促"的根因（离场继承了入场的 .2s 延迟；stagger 用
  class 会在过渡中重渲染时丢失 → 改内联样式）+ 离场节点改用 IDom::schedule 自己
  清理 + 侧栏搜索改图标跳转、主页搜索真正右对齐 + 会话设置（模型/档位/权限/
  自动挡）从 sessionSettings 回填 + 思考过程折叠块 + 工具块按索引折叠 + 复杂演示
  会话脚本（9 轮/7 思考/19 工具/5 语言/失败重试/切模型/计划）。214 用例 1928 断言。
  详见 current.md <50>。

<51>(SOLVED): 框架重排 bug —— `patch_children` 对每个子节点都 insert_before（已在位
  也会被摘下重插），导致滚动重置/动画重播/节点"重建"；改为"只插新节点"的子序列
  快路径 + DOM 侧 no-op 守卫。连带修好：规划改为独立列（不再重建 chat）、每会话
  滚动位置保存与首次滚到底、窄屏 100%-20px、工作过程折叠组（步数/成败/时长/
  结束时刻）、代码块语言+行号+复制、子代理可展开对话记录与耗时、终端单行不再
  可滚且行动画改为整块淡入。219 用例 1955 断言。详见 current.md <51>。


<52>(SOLVED): 语法高亮——不用 re2c（每语言 30-60KB DFA 表，几十种语言就 2-4MB），
  改为「一套手写扫描器 + 每语言一行 LangSpec 数据」：Flag 位掩码驱动通用扫描器，
  markup/css/json/markdown/diff 各一个专用分支，关键字懒建 set。输出是**一个
  innerHTML 字符串**（新增 VNode::html + IDom::set_html），不是上千个 span 节点，
  按 (语言, 内容哈希) 缓存 96 条。正确性靠 round-trip（拼接必须逐字节还原输入）。
  接入聊天代码块与文件预览（按扩展名猜语言，未知退回纯文本）；开关走 CSS，
  切换不重绘。234 用例 2370 断言。详见 current.md <52>。


<53>(SOLVED): 历史不重算（ChatMsg::rev + VNode 缓存 + 框架同指针短路）+ Git 面板接
  真后端（git CLI，status/diff/stage/commit，含未跟踪文件 diff 与 add -A 两个真
  bug）+ 用量面板去掉写死的 42%/¥0.21/12.4K，改用带 sid 的 usage.session。
  252 用例 2509 断言。详见 current.md <53>。

<54>(WIP): 界面四修 + 历史分页（前插不重建）+ 命令菜单后端化 + 上下文改精确值。
  宽度用 max-width 50% + container query 切 100%-20px（min-width 会永远赢，写不得）；
  规划回悬浮窗但仍不与 history 同层；侧栏切换改为双 view 错峰（修 _fs_ui_timeout
  未导出、错峰延迟被二次渲染清掉两个真 bug）；历史首屏 30 轮、上拉前插、
  消息身份改绝对轮次号 uid 使前插不重建、贴底才跟随；命令菜单由 command.list
  提供（含 skill），UI 零硬编码；思考/过程限 10 行半页可滚；上下文水位 =
  上次请求 prompt tokens / API 报告的 context_length。255 用例 2580 断言。
  current.md <54> 末尾附下一轮接线清单 <55>（21 项，按优先级分组）。
<56>(SOLVED): 21 项接线清单全部接上。核心前提：append_turn 从不写 turnId，
  fork/rollback/forget 三个接口其实是死的——已补。新增消息级分叉/回滚/忘记、
  中断常驻条+继续、挂起状态回读+取消唤醒、Enter 行为真正生效、切模型刷新运行时；
  Git 三视图（改动/历史/分支）+ HEAD 对照；用量导出 CSV + 跨 provider 模型列表；
  Skill 启停/删除/新建 + MCP 目录导入；设置恢复默认 + 项目忽略全局 + 思考语言下发；
  工作流侧栏面板（含新增 workflow.runs）+ 规划内联编辑/评审 + 终端编码。
  UI 覆盖 122 个方法中的 102，其余 20 个均为内部/模型侧接口。268 用例 2667 断言。
  详见 current.md <56>。

<57>(SOLVED): 打磨批。翻页后 summary 点不开的真根因是消息 handler 捕获数组下标
  （前插后整体位移）——改为按稳定 uid 解析；中断条现在显示后端失败原因；主页四页
  动画按用户要求全部回退为纯淡入（删掉用量页多出的 anim-enter）；紧凑时右侧按钮用
  margin-left:auto 靠右；新增紫点=后台在跑（terminalCount）；文件预览加行号与复制
  相对路径；修 IDom::schedule 自增两次导致退场节点永不清理的真 bug。
  268 用例 2672 断言。详见 current.md <57>。


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

<59>(SOLVED): Web / Desktop 双版本 + WebViewDom 适配器（webview 作为 WhaleUI 后备）。
  - 三变体共用同一套 UI，只换 IDom：Web(默认, WasmDom) / Desktop-webview
    (WebViewDom) / Desktop-WhaleUI(WhaleDom)；各自独立构建目录，build.bat 切换。
  - 新增 ui_dom_webview.cpp + ui_main_webview.cpp；webview 0.12.0 + WebView2 SDK
    入 3rdparty。三变体都内嵌 web/（Desktop 把 CSS 内联、woff2 转 data: URI，
    因为 NavigateToString 没有 base URL）。
  - 适配器要点：节点 id 由 C++ 分配；读操作走 C++ 镜像（fs_ui_push 回传）。
  - 坑：webview_init 必须在 set_html 之前；绑定是位置参数（C 侧收 JSON 数组）；
    insert_before 漏传 ref 会让重排全部追加到末尾（点击后整页变空）；
    链接需 shlwapi；xmake 的 io 只在 target hook 函数体内可见（打包逻辑必须内联）。
  - 详见 current.md <59> 与 doc-internal/ui-webview.md。

<60>(SOLVED): 构建目录归拢到 build/{web,webview,whaleui} + GitHub Actions 多平台打包。
  - 三变体命名统一（Web/Webview/WhaleUI），输出目录改为 build 下的分支；
    .gitignore 补齐，仓库根目录清掉 24 个历史日志。
  - 三变体都内嵌 HTTP 服务（远程入口仍 TODO）。
  - 跨平台坑：emcc 检测按 host 切 PATH 分隔符（否则 Linux/macOS 上 wasm 被静默跳过）；
    wasm target 补 set_arch("wasm32")（否则 arm64 交叉构建配置失败）；mbedtls 无预编译
    树时改走 add_requires（且此时不能带 3.6.4 头）；非 Windows 加
    _DEFAULT_SOURCE/_DARWIN_C_SOURCE + pthread/dl；webview Linux 走
    pkgconfig::gtk+-3.0 + webkit2gtk-4.1，macOS 走 -framework WebKit + dl。
  - scripts/fetch_3rdparty.py 统一依赖拉取（幂等），scripts/package.py 出包，
    .github/workflows/build.yml 矩阵（win x86_64/arm64、macos arm64、linux x86_64/aarch64）
    按 tag/release 触发，每平台每变体一个包。详见 current.md <60>。

<61>(SOLVED): 网络"连不通"的三个真 bug + 多轮对话 + 定时任务。
  - httplib 的 TLS 宏只在 fs_core 上定义，ui_server.cpp（fs_ui）编出另一份非 TLS
    实现且被链接器选中 → 所有 HTTPS 请求 "Could not establish connection"。
    宏提到项目级。
  - mbedtls 链了 MSVC 编的 .a，两套 CRT 混用 → 请求完成时 RtlFreeHeap SIGSEGV。
    改为 mbedtls_src 从源码编。
  - provider 路由只取第一个启用的连接，deepseek 的模型被发到 anthropic → 403。
    改为按模型归属匹配 provider。
  - 多轮对话是假的：HistoryBuilder 没被调用、persist_turn 从未调用。已接上。
  - 失败后 state 停在 kTurnDone，被当成"忙"→ 后续消息全进引导队列。已修。
  - 换模型不生效（loop 只读一次模型）；定时任务无创建接口且 on_timer_tick
    从未被调用。补 session.schedule/pendingWakes + UI 倒计时条 + 定时按钮。
  - 输入框 placeholder 跟随 Enter 设置。详见 current.md <61>。

<62>(SOLVED): 重开会话看不到用户输入 + 全局右键菜单。
  - user turn 把输入存在 `promptRaw`，UI 只读 `text`/`streamSummary`，所以用户
    消息全渲染成空白（答案在、问题没了）。改为三级回退。
  - 浏览器原生右键菜单是文档级默认行为，只绑节点拦不住；两个适配器改在
    document 上监听，文本输入区放行（保留原生剪切复制粘贴），其余拦截。
  - 应用菜单：消息=复制/分叉/回滚/忘记，标签页=重命名/关闭，会话行=打开/重命名/
    删除，输入区=清空/复制。坐标来自指针（适配器把事件值改成 "x\ty"）。
  - 详见 current.md <62> 与 doc-internal/ui-events.md §3.1。

<63>(SOLVED): 真流式（SSE）+ 计时 + 右键补齐 + 上下文水位 + 模型属性自动填充。
  - 流式原本是假的（`stream=false`）。现在 NetClient 有 request_stream、ProviderClient
    用 SseReader 解 SSE、AgentLoop 边收边发 stream.delta；非流式仍兼容。
  - UI：delta 判断漏了 `!streaming`（新答案粘在旧消息上）；每条 delta 都 render 会
    饿死事件循环（看着像卡住）→ 改 50ms 节流；poll 加在途保护、每批 128 条、事件环 8192。
  - 计时：流式显示已用时长，结束后常显"时长 · 结束时刻"（后端给 startedAt/endedAt）。
  - 右键：左栏会话行、输入框都接上；菜单外任意按下关菜单且不吞事件（第二次右键切换菜单）。
  - 水位：流式要 stream_options.include_usage、resolve 合并而非覆盖 API 的 context_len、
    取回的模型列表落盘、最低显示 1% 并常显数字。
  - 模型属性：一次获取列表就填好 name/context/max_output/多模态/档位/能力。
  - 详见 current.md <63>、doc-internal/ui-events.md §3.2、doc-internal/provider.md §4。

<64>(SOLVED): 流式"像一次性生成完再喂" —— 浏览器定时器节流。
  - 页面里 `setInterval(120ms)` 实测间隔 1031ms（被遮挡/后台页面被压到 ~1Hz），
    而模型每秒产出约 150 条 delta，轮询每次最多 128 条 → 永远追不上，生成结束才补完。
  - 改为服务端 SSE 推送（`GET /api/stream`，chunked + cv 唤醒）+ 前端 EventSource；
    重绘也改成 delta 到达即 render（SSE 帧不受定时器节流），25ms 下限防连发。
  - 实测 47 次重绘、字符数平滑增长、计时 0.0→1.6s；结束后常显"2.7s · 09:19"。
  - 教训：UI 刷新节奏若由定时器驱动，先确认它没被浏览器节流。详见 current.md <64>。

<65>(SOLVED): 模型可用性校验 + Web 服务设置（远程 + 密码）+ 密码遮罩/鉴权。
  - 会话记住的模型失效（被禁用/供应商被删）→ 清空模型框、标警告、禁用发送；
    send_message 同一道闸；无后端时跳过（测试/预览）。
  - 设置-权限-Web 服务：监听地址/端口/密码，改动实时生效（stop+start 重绑）。
    重启必须放 detached 线程：在请求线程上 stop() 会 join 到线程池里的自己 → 死锁。
  - 鉴权只约束远程，回环免密；/api/auth 探测、/api/login 换 token、/api/* 带 Bearer；
    页面外壳（/、css、js、wasm）必须公开，否则连密码框都加载不出来。
  - 密码遮罩是 index.html 的纯 HTML/JS；登录成功后要显式 startApp()，否则界面空白。
  - 顺带修：fs_ui 未声明 web_assets 为输入，改 HTML/CSS 不重新打包（内嵌资源是旧的）。
  - 详见 current.md <65>、doc-internal/ui-webview.md §1.1、provider.md §3.1。

<66>(SOLVED): 三种访问方式并列（只有 Web 版跑服务）+ 端口修改真正生效。
  - Webview / WhaleUI 有原生窗口，不再启动内嵌服务；只有 Web 版需要（服务就是 UI）。
  - 端口"没生效"是因为 on_input 逐字符提交，逐位重绑、互相踩掉；改成只在 Enter 提交，
    输入中只更新本地 draft。
  - 换端口后旧页面必然失联：重绑前推 `server.moved`，页面显示"服务已更换地址"+
    新地址链接。
  - restart 是异步的，加 `rebinding` 标志让设置页延迟重读状态，避免读到旧端口。
  - 详见 current.md <66>。

<67>(SOLVED): 重绑不换端口 + 换端口时页面自动跳转。
  - 重绑沿用了 `port = 0` 那条路，导致只改密码/地址也会换端口。改为只有明确写不同
    端口才换。
  - 重绑改成先绑新端口再关旧监听（中间无监听窗口会让页面轮询失败）；绑不上就保留原
    监听并在设置页显示错误。路由注册抽成 configure()，重绑对新实例重放。
  - `server.moved` 只带端口，URL 由页面用自己的 hostname 拼；收到后先探测新端口，
    通了才 replace（立即跳会落到浏览器错误页）；探测失败才出提示卡片。
  - "Web 服务"设置页只在 Web 版显示。详见 current.md <67>。

<68>(SOLVED): Web 服务改端口/改地址真正生效（根因：SSE 把 stop() 卡死）。
  - 页面常挂的 `/api/stream` 一条 SSE 会一直占着线程池里一个 worker；httplib 的
    stop() 要 join 线程池，于是重绑永远卡住、旧监听不释放——这就是"改端口没反应
    / 0.0.0.0 没有服务"。加 `stopping` 标志：stop() 置位并唤醒，SSE 处理器见信号
    立即 return false 结束请求；keep-alive 等待 15s→0.5s。
  - 端口框 Enter 或失焦都提交；输入非法值（非数字 / 超 0-65535）拒绝并提示，不再
    静默钳到 65535。设置页显示真实局域网 IP（UDP connect 8.8.8.8 读 getsockname）。
  - 详见 current.md <68>。

<69>(SOLVED): 改端口/切地址闪退 + 页面失联 + 端口"自己变了"。
  - 闪退根因：`restart()` 每次新开 detached 线程；连续两次设置 → 两个线程同时
    `stop()`，第二次 `join()` 已 join 的 `std::thread` 抛异常，detached 线程里没人
    接 → `std::terminate`（无输出退出）。改成单个常驻 worker 串行合并重绑。
  - 页面失联：原来先关旧监听再起新监听，中间空窗导致旧页面探测失败死在原地址。
    改成新监听先 accept，再退役旧监听；端口变化时旧 socket 多留 1.2s。
  - 停止标志按"代"隔离（每次重绑新建 `shared_ptr<atomic<bool>>`），否则退役旧监听
    会把新监听的 SSE 一起杀掉。listen 线程改为按值捕获 Server*，不再读 impl_->svr。
  - `--port` 现在写回 `server.port`：此前命令行端口只用于本次启动，之后改地址会用
    配置里的旧端口把服务器悄悄搬走——这正是"本地也变了"。
  - 详见 current.md <69>。

<70>(SOLVED): 变体构建全在 xmake 里 + webview 资源没嵌入 + tool call 全链路。
  - 删掉 `build.bat`：三个变体改为 xmake 单值选项 `--variant=web|webview|whaleui`。
    两个布尔开关会同时留在缓存里（`--whaleui=y` 不清 `--webview`），于是"选
    WhaleUI 出来 Web 包"。产物名带变体，同目录共存；`package.py` 只认对应名字，
    不再 fallback 到任意 `flashsight*`。
  - 切变体只重编 3 个文件：变体宏从项目级 `add_defines` 挪到 `fs_ui_host` 与
    `flashsight` 两个目标；`fs_ui` 不再含变体宏。实测 7 秒。
  - webview 资源没嵌入：资源头以前写进 `src/ui/` 且所有变体共用文件名，后编的
    覆盖前一个。现在在 `build/gen/`（跨变体固定路径，避免 include 路径变化导致
    全量重编），且内容不变就不写盘。
  - 依赖边修正：`fs_ui→fs_core→mbedtls_src` 各自 `add_deps`，否则并列依赖会被
    按名字排序，归档顺序错开导致 `fs::highlight::*` / `mbedtls_*` 未定义。
  - tool call：`tool.start`（带 args，立即显示"运行中"）→ `tool.update`（summary
    + detail，**原地补全**同一行）；`ToolBlock.running` 用转圈代替对勾；落库存
    真正执行过的调用；回填补上 tool_call id。新增 2 个用例。
  - 详见 current.md <70>。

<71>(SOLVED): GitHub Actions 重写（变体选择 + 资源断言 + 可选 WhaleUI job）。
  - 构建参数改为 `--variant=web|webview`（上一版 workflow 还写着 `--webview=y`
    和 `build/web`，等于没跟上本地改动）；两个变体同一个 job 先后编。
  - `workflow_dispatch` 增加 `variant` 输入，可只编一个变体。
  - 资源断言：二进制必须含 `#fs-root` + 样式 + Web 版的 `fs_ui.wasm`；webview
    版**不能**含 wasm（含了说明用错 pack）。查找按 `build/<plat>/<arch>/release`
    与平台后缀，macOS/Linux 也适用。包数量断言跟随变体选择。
  - WhaleUI 独立可选 job（`ENABLE_WHALEUI=true` 打开）：clone WhaleUI → 编
    `libwhaleui-full` → 装 xrepo 依赖 → 编 FlashSight；库路径用 `find` 定位。
    release 用 `always()` + result 检查，跳过时不挡发布。
  - 详见 current.md <71>。

<72>(SOLVED): Agent 上下文重做（前缀/对话/每轮状态三层 + 中性消息格式 + 三形态适配）。
  - **根因**：`AgentHost::loop_for` 从未给 `AgentLoop` 填 `history.tools / env /
    system_prefix`，模型收到的是 `tools: []` 和空系统提示——"agent core 太简单"的
    真实原因不是提示词写得少，而是**根本没有提示词**。
  - **分层**（doc-internal/agent-context.md）：稳定前缀（身份/工作方式/规划与工作流
    建议/工具索引/skills/环境/项目约定/长期偏好/思考语言）→ 对话（中性消息）→
    每轮状态块（时间/规划/清单/子代理/水位，追加在最新 user 消息里）。
    易变字段一律不进前缀，否则每轮前缀都变、前缀缓存全废。
  - **规划与工作流显式建议**（用户点名要求）：每块都给"什么时候用"和**"什么时候
    不用"**（todo 用于一次性多步、workflow 用于要反复跑/逐步重试的流水线、子代理
    的判断标准是"上下文成本"）。同时补了 `plan_update/get`、`subagent_spawn/list/
    cancel/steer`、`workflow_list/start` 八个工具——此前提示词描述的能力在工具表里
    一个都够不着。
  - **中性消息格式 + `provider::prompt::adapt`**：循环不再产出 provider 形状。
    旧实现把 Anthropic 的 `{type:"tool_result"}` 塞进 user 消息发给所有端点，
    `[OI]` 端点忽略它 → 模型看不到结果 → **无限重发同一调用**。现在按
    `[OI]/Anthropic/Responses` 分别渲染，Anthropic 还补了"相邻同角色合并"和
    "首条必须是 user"的修复。
  - **整组工具调用**：一条回复里的多个调用逐个执行、每个单独一条 `tool` 消息；
    审批门按整组保存（`gate_messages_`），批准后继续执行**剩余调用**（旧实现丢掉了
    它们）。参数 JSON 非法时**不执行**（否则"写文件"变成"清空文件"）。
  - **边界**：单轮往返 60 / 连续相同调用 3 / 自动续跑 12，触发就结束并注明原因。
    自动续跑必须把模型上一条回答放进历史，否则像"用户凭空说继续"。
  - **历史**：预算从**最新往回**算（旧实现正序累加、一超预算就 break，把最新的对话
    全丢了）；工具结果按 assistant→tool 顺序回放；折叠摘要带 keyPoints。
  - **子代理真正执行**：宿主 `run_subagent` 起独立 `AgentLoop`（不读父历史、不落库、
    只读代理只拿只读工具、权限不超过父），worker 线程跑完回摘要；宿主析构 join 线程。
  - **UTF-8 坑**（排查了很久）：`find_first_of("。.\n")` 匹配的是**字节集合**，`。`
    的 0x80/0x82 也是别的汉字的续字节 → 切出半个汉字 → `json::dump()` 抛
    type_error 316，整轮崩。同理 `substr(0,4000)`。新增 `common/fs_utf8`（`truncate`
    只切字符边界、`sanitize` 兜底），所有截断改走它。
  - 新增 25 个用例（`test_context.cpp`、`test_loop_flow.cpp`），297 用例 2891 断言全过；
    web / webview 两个变体编译通过。
  - 详见 current.md <72>、doc-internal/agent-context.md。

<73>(SOLVED): 水位驱动压缩 + edit_file 协议 + 用量口径 + 按步骤派子代理。
  - **压缩改水位驱动**：`should_compress()` 独立可测，三触发——水位
    （`history.compressAtPct`，默认 80，读上一次 prompt tokens / 模型窗口）、
    预算（`tokenBudget` 32000，窗口未知兜底）、强制（用户点压缩）。
    关键修复：旧实现只把预算调小，短会话仍放得下 → "点了压缩没反应"；现在决定
    压缩就必然保留 `keepTurns` 条并折叠其余。水位从用量日志读回，重启后仍有效。
  - **edit_file 协议**（移除 `file.diffWrite`）：read 返回整文件 `hash`（xxh64 前
    12 位），edit 带 `hash` + 每段 `expect` 双校验。hash 不符 = 文件被改过
    （返回 `currentHash`/`stale`）；expect 不符 = 行号对但内容不对（返回 `actual`）。
    多段按原始行号从下往上应用、重叠拒绝、越界拒绝、空 newText = 删行。
    **测试抓到真 bug**：旧实现每次编辑吃掉文件末尾换行 → 已用 `final_newline`
    显式保留；CRLF 文件编辑后仍 CRLF。
  - **read 守卫**：>32MB 拒绝、>1MB 需显式分页、单行 >4000 截断并标记、NUL 判二进制。
  - **用量口径**：缓存命中率分母只算上报过 cache 的行（未上报 = -1 而非 0%）、
    按 provider 区分 money/quota/unknown（混加是错的）、`recent[]` 给 UI 算"本轮"。
    侧栏补：绝对值水位、缓存命中、本轮/会话成本、调用次数、超阈值时的压缩按钮。
    设置页键名从不存在的 `history.compressPct` 统一为 `history.compressAtPct`，
    并补保留轮数 / token 上限 / 立即压缩。
  - **按步骤派子代理**：前缀明确"清单建好后逐项 spawn complete-step（step/todoId）"，
    子代理上下文只做那一项；`subagent_steps_` 完成时自动 `todo_check`。子代理事件
    带 `subagentId`/`fromSubagent`：主界面可见过程，但不进 full.json、不进父会话历史。
    无人值守（unattended）时需批准的动作直接拒绝并给 `howToProceed`，不会挂死。
  - 新增 `common/fs_hash`（xxh64）+ `tests/test_agent_v2.cpp`（19 用例）；
    318 用例 3008 断言全过；web / webview 均编译通过。
  - 踩坑：PowerShell `Get-Content`/`Set-Content` 改 .cpp 会按 ANSI 破坏 UTF-8 中文，
    已回滚改用 Edit 工具。
  - 详见 current.md <73>、doc-internal/agent-context.md §5/§7.1/§10.1/§10.2。


<74>(SOLVED): "任务已中断 http 400" —— 工具名非法 + 错误体被丢。
  - **根因一**：OpenAI/Anthropic 要求工具名匹配 `^[a-zA-Z0-9_-]{1,64}$`。上一轮把
    工具表真正发给模型后，`file.read`/`plan.update` 的点号让**每个请求**在到达模型
    前就被 400 拒绝。37 个工具全部改成下划线：`read_file`/`write_file`/`edit_file`/
    `list_dir`/`find_files`/`remove_file`/`terminal_exec`/`fetch_url`/`todo_add`/
    `plan_update`/`subagent_spawn`/`load_skill` 等。
  - **两层防护**：`register_tool` 用 `wire_safe_tool_name()` 规范化（`.`/`/`→`_`）
    并同步 `ToolDef.name`，注册表键/schema/回调名永远一致；`test_wire_names.cpp`
    遍历真实注册表 + 三种 provider 形态断言合法性。
  - **根因二**：`provider_client` 把非 2xx 压成 `"http 400"`，丢掉了端点返回的
    `tools[2].function.name: does not match pattern ...`。新增 `http_error_text()`
    解析 `{"error":{"message"}}`、非 JSON 用原文、单行截 400 字符。
  - 区分：事件名（`subagent.done`）、FsApi 路由（`plan.update`）、设置键
    （`history.compressAtPct`）不进请求体，保持点号。
  - 顺带：`file_regex` 曾误在只读子代理白名单（它会写文件）→ 已移除，新增
    `read_only_agent_has_no_write_tools` 用例。
  - 325 用例 3313 断言全过；web / webview 编译通过。
  - 详见 current.md <74>、doc-internal/agent-context.md §2.5.1/§10.3。

<75>(SOLVED): 图片/搜索真接线 + 桌面端流式卡死。
  - **卡死根因**：`NativeBackend` 没重写 `call_async`，用接口默认实现=同步调用。
    桌面版每次 `session.send` 在 UI 线程跑完整个 turn → 窗口冻住、delta 全排队、
    完全不流式（浏览器版 HttpBackend 天生异步，所以没暴露）。改成 FIFO 队列 +
    **单个**工作线程（FsApi 非线程安全，多线程会踩 map），结果/事件共用队列，
    `poll()` 在 UI 线程交付。
  - **图片**：`common/fs_screen` 三平台截图（GDI/CoreGraphics/X11，Wayland 明确
    报不支持）+ `common/fs_image` 自写 PNG 编解码/缩放/base64。**关键**：工具返回
    `dataUri`，loop 取出变成中性 `parts` 块，adapter 渲染成 OpenAI `image_url` /
    Anthropic base64 source——只返回路径模型看不见。Windows 非交互会话全黑时
    报错而非喂黑图。
  - **搜索**：`tools/search_backend` 支持 tavily/brave/searxng/custom，独立
    `web_search` 回调（原先把 `{search:true}` 塞进 `net_fetch`，那回调只读 url
    → 每次 "empty url"）。设置页可配 provider/key/endpoint。
  - 新增 21 用例；346 用例 3419 断言全过；web / webview 编译通过。
  - 详见 current.md <75>、doc-internal/agent-context.md §10.4。

<76>(SOLVED): 桌面端卡死重做（线程池 + 边界锁）+ 截图 DXGI 优先 + 图片改用 stb。
  - **卡死重做**：上一版"单工作线程串行"能跑但一个长 turn 期间所有调用都排队。
    现在：工作线程池（按需到 4）+ UI→后端边界一把递归锁（130 路由一次覆盖，
    新增自动受保护）；`session.send`/`tool.invoke` 不在这把锁里（它们阻塞很久）；
    turn 回调里的**嵌套调用内联执行**（thread_local 识别，排队会死锁）；
    `AgentLoop` 的 state_/suspend/saved_valid/compress 改 atomic（UI 每帧读）。
  - **截图**：Desktop Duplication（D3D11+DXGI）优先，GDI BitBlt 兜底——前者读
    DWM 合成表面，锁屏/未连接 RDP/多 GPU/HDR 下后者全黑而它正常。本机实测
    Duplication 不可用 → 自动回落 GDI 并成功截到 2048x1280。
  - **图片改用 stb**（`3rdparty/stb`，三个单头）：stb_image 解 PNG/JPEG/GIF/
    BMP/TGA/PSD/HDR（WebP 明确报错）；stb_image_resize2 做 sRGB 感知缩放
    （文字仍可读）；stb_image_write 出真压缩 PNG。解码前 `stbi_info` 读头部 +
    `kMaxPixels` 拦解压炸弹。已加进 fetch_3rdparty.py。
  - 347 用例 3421 断言全过；web / webview 编译通过。
  - 详见 current.md <76>、doc-internal/agent-context.md §10.4。

<77>(SOLVED): ask 静默失败 + 自动轮无预告/无圆点/混历史 + 失败轮丢输入 + 工具上限措辞。
  - **ask 根因**：工具发事件后返回 `asked:true`，模型以为已答，选择从未回填。现在
    工具只返回 `awaiting_user`，**loop 挂起本轮**；`answer_ask` 把选择作为该工具
    结果回填并续跑；过期 askId 拒绝。
  - **ask 界面**：原来复用权限框（只有允许/拒绝），选项没显示。现在独立弹窗：
    问题 + 选项（标推荐）+ 自由输入；关闭也回填 "(dismissed)" 不挂死。
  - **自动轮**：`auto.next` 带 round 并弹 toast 预告；`session_dot` 加 auto_round
    参数走紫色 background 分支；`session.list` 回 `autoRound`，主页可见。
  - **不混历史**：自动轮存档带 `autoRound`+`system:true`，渲染成 system 分隔条，
    加载时不再被 user/assistant 过滤掉。
  - **失败轮丢输入**：`run_loop` 首发失败时先 `persist_user_turn_only()` 再返回。
  - **工具上限**：默认 60→200，措辞改"已暂停，可继续"，事件 `loop.interrupted`
    → `loop.paused`（不再冒充失败）。
  - 新增 6 用例；353 用例 3455 断言全过；web / webview 编译通过。
  - 详见 current.md <77>。

<78>(SOLVED): ask 面板重做 + SYSTEM 前缀 + round limit 自动开新轮 + 成本估算 + markdown。
  - **ask 面板**：从浮层对话框改为**占据输入框区域**：标题(含 1/x) + 问题正文 +
    选项行（单选圆点+推荐标记）+ 自定义输入 + 发送按钮。点选项只是选中，
    必须点"发送回答"（用户要求手动发送）。多提问合并成一个 `ask.prompt`（`asks[]`），
    全部答完才回填续跑。
  - **SYSTEM 前缀**：自动续跑 nudge 改为 `SYSTEM: ...`；存档带 autoRound+system，
    渲染成系统分隔条不是气泡。
  - **round limit**：达上限后**自动开新一轮**（不再等用户），nudge 明确说明是
    "系统自动切分，不是用户要求停止、不代表完成，请先审视进度"。上限 200 作为安全网。
  - **成本**：模型能力加单价（每百万 token in/out/cache）；优先级 端点cost >
    usage.cost > 单价×token 估算(标 estimated) > quota > unknown；缓存字段兼容
    DeepSeek `prompt_cache_hit_tokens` 与 `prompt_tokens_details.cached_tokens`；
    UI 显示 `≈¥x（估算）` 不冒充账单。
  - **markdown**：新增引用、分隔线、表格、链接；链接点击由 `link.open` 决定：
    http(s)→系统浏览器，项目内路径→侧边栏预览，file:///自定义协议拒绝。
  - 358 用例 3489 断言全过；web / webview 编译通过。

<79>(SOLVED): 子代理 403 + 压缩设置不生效 + MCP/Skill 混淆 + 工具结果投影 + i18n 补齐 + 快捷键接线 + 弹窗失焦 + 滚动状态。
  - **子代理 403 根因**：`send` 用空的 `context().model` 建上下文 → seed 里模型为空 →
    provider 路由回退到"第一个启用的连接" → DeepSeek 发到 Anthropic 得 403。改读 `loop->model()`。
  - **压缩设置不生效**：`history.*` 只在 loop 创建时读一次；现在每轮 `set_history_options` 重读。
  - **MCP/Skill 混淆**：`mcp_detect` 探测的是 skill 目录并注册成 MCP 条目（用户配置里 72 条
    `gsd-*` 全是这样）。现在 MCP 探测读 `mcpServers` 服务器定义、Skill 有自己的 `skill.detect`；
    条目带 kind/server；启动时把落在 skills 目录的旧 MCP 条目迁移进 skill 注册表（实测清成 0）。
  - **工具结果投影**：`project_tool_result` 按工具保留"能继续"的字段、丢掉 bulk（read_file 正文
    900 字符、edit 只留行区间、终端取尾部、图片不留 dataUri）；老格式先试解析 JSON，失败保留截断文本。
  - **i18n**：扫出 tr 键缺 en 条目 124 条 + 未套 tr 的可见文案 7 处，补齐；侧栏标题/说明过 tr。
  - **快捷键**：`bind_global_key` 改带修饰符（`"Ctrl+W"`），三适配器都改；设置表真正被读取
    （关/建标签页、Enter 行为）。
  - **弹窗失焦**：模态绑 `bind_outside_click`；权限门"关闭=拒绝"（否则挂住整轮）；修了适配器里
    豁免整个父容器导致"关不掉"的 bug。
  - **滚动状态**：webview `set_scroll_top` 同步回写镜像；滚动时持续更新 offset。
  - **子代理分流**：事件带 `subagentKind`，complete-step 进主会话、其余进侧栏。
  - 新增 6 用例；363 用例 3506 断言全过；web / webview 编译通过。详见 current.md <79>。

<80>(SOLVED): 子代理线程栈泄漏 + 弹窗失焦 + 规划不实时 + 子代理面板/follow-up + 历史分片 + 主页驻留。
  - **内存泄漏（最要紧）**：`subagent_threads_` 只增不减，跑完未 join 的 `std::thread` **仍占着
    线程栈**（Windows 每线程保留 MB 级），而它只在析构时被清——**每个子代理泄漏一份栈**。
    新增 `reap_subagent_threads()`：按记录状态判完成 → 不持锁 join（持锁会与该线程最后的 emit
    死锁），spawn 前与 tick 时调用；`thread_local` 防嵌套 spawn join 到自己。
  - **弹窗失焦**：三个适配器只保留**一个** outside 回调（多弹窗互相覆盖），且文档级监听器不随
    节点移除 → 旧弹窗的监听器去调当前回调，关错对象/关不掉。改为**按节点 keyed 的回调表** +
    JS 侧 `isConnected` 判断；右键菜单改用 key 查节点（原来缓存的节点在菜单重建后已脱离）。
  - **规划**：宿主发 `plan.update` 但 **UI 从没监听**（只在点按钮时 `plan.get`）→ 规划看不见。
    现在实时更新；`plan_steps` 带 `status/note`，面板显示状态与 `已完成 n/总数`（与
    `should_auto_continue` 同一条件）；**删掉手动增删改**（规划是模型的工作状态，保留评审）。
  - **子代理**：侧栏显示运行中的 `live_text`、转录补上 tool 名字；「引导」改输入框——运行中
    `steer`、**已结束 `followUp`**（同 id + 原转录续跑，工具 `subagent_follow_up`、
    路由 `subagent.followUp`、`seed_messages` 回放）。工作过程 50vh→70vh、工具详情 220px→60vh。
  - **历史分片**：`full.json` 每轮整份重写（O(n)/轮，6 轮 90KB）→ `turns/NNN.json`（16 轮/片），
    `load_full()` 仍返回完整 turns（上层零改动）；`forget` 原本直接写 full.json 被这次抓出修正；
    回滚删多余分片；**老格式可读且下次追加自动迁移**（判定"存在分片文件"而非"存在目录"）。
  - **顶栏标签**：拆成 `tabs-fixed`（主页）+ `tabs-scroll`（会话 + "+"），主页永不被推出屏幕。
  - 366 用例 3553 断言全过；web / webview 编译通过。详见 current.md <80>。

<81>(SOLVED): 急性卡死（定时器持锁 join + 子代理 delta 每帧重绘）+ spawn wait 无效 + list 撑爆 + 子代理独立标签。
  - **卡死不是慢性泄漏**（用户纠正：生成子代理后几秒内发生）。两个真根因：
    ① `on_timer_tick()` **持 host 锁**调 `reap_subagent_threads()`，它 join 已完成子代理线程，
    而该线程最后一步 `emit()` 要同一把锁 → 定时器等线程、线程等锁；tick 60ms 周期所以像定时冻住。
    改为 reap 在取锁之前（写进函数契约）。② 上一轮加的"侧栏实时内容"**每个 delta 都全量 render**，
    子代理一开口每秒上百 delta 把事件循环饿死（主对话早有 25ms 节流，这条路径漏了）→ 改为置脏位由
    pump() 画。③ 顺带修子代理收尾块**无锁写 `records_`/`subagent_steps_`** 的数据竞争。
  - **`spawn(wait=true)`** 以前完全没读参数；现在轮询记录直到报回（不是 join，否则重现死锁）并返回 summary。
  - **`subagent_list` 撑爆**：每项内嵌 ~10KB system prompt，3 个就截断。`to_json()` 不再带转录，
    只给条数；完整记录走 `subagent.detail`。
  - **子代理记录**：侧栏卡片**可点** → 独立标签 `sa:<id>` 看完整转录；工作过程里
    `subagent_spawn`/`follow_up` 行解析出 id 并给「打开记录」。解决"一结束就永久看不到"。
  - **中途输出**：工具轮之间的旁白以前既不落库也被粘进答案 → 新增 `assistant.interim` 事件，
    归入工作过程"过程说明"并随 turn 落库（`turn.interim`）。
  - 工作过程 78vh、单次调用 65vh（过程内 70vh）；`cancel` 区分运行中/已结束。
  - 369 用例 3572 断言全过；web / webview 编译通过。详见 current.md <81>。

<82>(SOLVED): 卡死真因（follow_up 持锁跑 loop + collect_runtime 无锁读）+ 工作过程不显示 + 记录改回侧栏。
  - 用户线索"**spawn 第二个子代理才出现**"是钥匙：① `subagent_follow_up` **持 host 锁跑整轮
    agent loop**（emit 要锁、嵌套 spawn 也要锁）→ 第二个代理一出现就硬锁死；改为锁内只快照、
    释放后再跑。② `collect_runtime()` **无锁读 `records_`/`todos_`**，而子代理线程并发写 →
    `std::map` 数据竞争（崩或自旋）；单个代理时没有并发写者，所以"第二个才出现"。③ `records_`
    永不清理（每条带整份转录 + 每条消息一份 system prompt）→ 每会话几 MB，最后自己退出；
    新增 `prune_finished(sid,40)` 在 tick 里剪，运行中永不剪。
  - **工作过程不显示**的真因：模型常**只发工具调用不发文本** → 没有 `stream.delta` → 没有
    assistant 消息，而 `tool.start/update` 要求 `m.back().role=="assistant"` → 工具块被丢弃；
    同一个原因导致**计时器不显示**。新增 `ensure_turn_message()` 按需创建；工作过程渲染门槛
    加上 `interim`。
  - **高度全部去掉上限**（10 行/28em/78vh/220px/65vh 都试过，都难用），随内容增长由页面滚动。
  - **子代理记录改回侧栏**（`kind="subagent"` 侧栏标签，可多开可关闭）：全屏页关不回会话，
    用户明确要求放侧栏；卡片点击与工作过程「打开记录」都指向它，已结束的照样能看。
  - 372 用例 3589 断言全过；web / webview 编译通过。详见 current.md <82>。

<83>(SOLVED): 闪退丢记录（live 快照落盘）+ webview 关闭崩溃 + 挂起等后端 + 发送变中止 + 思考档位接线。
  - **闪退丢记录（真 bug）**：回合只在**结束时**才写入会话，进程一崩整轮就没了；此前的 live
    快照只在内存里，崩溃正好一并带走。现在流式期间每 ~2s 把在飞回合写进 `runtime.json` 的
    `live`（正文 + userText），落地时清空；`session.liveTurn` 读回时标 `crashed:true`，
    UI 渲染成已完成消息并提示已恢复。
  - **`Chrome_WidgetWin_0` 崩溃**：`webview_dispatch` 的任务捕获 `&ui`，`webview_run()` 返回后
    队列里仍有未执行任务 → 在 `webview_destroy()` 期间访问已析构对象。改为
    `PumpCtx{app, alive}` + `shared_ptr<atomic<bool>>`（按值），关闭顺序
    running=false → join → alive=false → app=nullptr。
  - **思考档位"没做"**：用的是硬编码**模型名白名单**，自定义/新模型返回空 → 控件不渲染。
    改用模型表的 `thinking.levels`。
  - **挂起**只发请求、回读 `session.loopState` 决定文案（请求在飞时禁用）；
    **发送 ↔ 中止同键**（`turn_running`，中止走 suspend）；**模型按钮带供应商**；
    **侧栏记录可滚动**（flex 子项 `flex:none`）；**去掉手动"定时唤醒"**（排程是模型的动作）。
  - 374 用例 3583 断言全过；web / webview 编译通过。详见 current.md <83>。

<84>(SOLVED): 思考档位终于出现（无档位模型回退 disable/enable + 出网归一化）+ 补全可用（Tab 真的能接受）+ markdown 提示条从不显示的真因 + ui_app.cpp 批量误删后的恢复。
  - 真因：用户的模型配置就是 `caps.thinking=false` + 空档位表，而 UI 只在"档位表非空"时渲染。现在空表回退成 **关 / 开**，默认项仍是"默认"（什么都不发）。
  - 回退值不能直接出网：新增 `wire_thinking()` —— `disable/off` 省略字段、`enable` → `medium`、其余透传；Anthropic 补 minimal/max 预算，Responses API 补 `reasoning.effort`。
  - Tab 接受补全：`accept_complete()` 只改 state，而框架故意不给聚焦的 textarea 回写 value → 文本从未进 DOM。现在直写节点，并在 `data-keys-capture` 存在时对 Tab/↑/↓/Esc `preventDefault`。
  - markdown 提示条：检测代码原本写在 `build_session()`（只在 render 时跑，而打字不重绘）→ 从未执行。拆出 `maybe_update_md_hint()` 在输入回调里调用，仅在识别集合变化时重绘一次。
  - 设置页只剩 `自动压缩阈值` + `保留最近轮数`；"最近轮次"窗口与"token 硬上限"（每轮都触发的隐形压缩）已删除，并加了防回归用例。
  - 另：从一次自造的批量替换事故中恢复 `ui_app.cpp`（token 级对齐 HEAD 还原 24 行 + 编译器逐条修 tr() 包裹与括号）。教训：绝不对源码做"先删再补"的批量正则。
  - 380 用例 3611 断言全过；web / webview 双变体编译通过；浏览器实测思考控件 `默认/关/开`、输入行单行 28px 无溢出、紧凑模式隐藏厂家、`/` 菜单 ↑/↓ + Tab 插入 `/new `、`./sr` 弹 `src/`、markdown 提示条显示 `bold · inline code`。详见 current.md <84>。

<85>(SOLVED): 思考档位按模型配置（去掉伪"默认"）+ 空图标按钮 + 删掉 markdown 提示条 + 子代理记录落盘 + 滚动恢复延后。
  - 下拉框只列模型表里的 `thinking.levels` 并预选 `thinking.default`；`disable/enable` 只在模型
    没有任何档位时作为 fallback，伪"默认"选项删除。
  - 空按钮真因：`thinking_icon()` 返回的 `lightbulb` 不在图标子集里，渲染成空白。
  - `save_model` / `toggle_model_enabled` 之后补 `refresh_model_availability()`，否则改完模型设置合成器不变。
  - markdown 提示条整套移除（textarea 无法局部上样式，而"打字不重绘"是硬约束）。
  - 子代理记录：toolCall 写 `subagentId`、重建时读回、完成时落盘 `<session>/subagents/<id>.json`，
    `subagent.detail` 在内存 miss 时读盘。
  - 顺手删掉死字段 `recent_turns` / `token_budget`。384 用例 3635 断言全过。详见 current.md <85>。

<86>(SOLVED): 思考档位只剩一个控件（每档一个图标 + 未知用灯泡）+ 挂起/恢复真的能继续 + 模型选择器打开在对应页 + 页面切换滚动改同帧恢复。
  - **重复的思考档位**：一个没有 handler 的图标按钮 + 一个显示同一值的原生 `<select>`，看起来就是
    两个控件、其中一个按了没反应。现在只有一个图标按钮，点开是档位列表（每行自己的图标 + 勾选）；
    窄容器自动只剩图标。
  - **每档一个图标**：关=power-off、最低=feather、低=gauge、中=brain、高=fire、超高=gauge-high、
    最大=infinity、自动=wand-magic-sparkles，未知档位统一灯泡。重新生成图标子集时发现 `fa-brain`
    原先指向错误码位（f19c → f5dc），旧图标零丢失（脚本对比）。
  - **挂起"没做全"三处真缺陷**：停车后 `loop_state()` 报 idle（UI 立刻退回"挂起"）→ 新增 `parked_`；
    `resume()` 对已停车的循环是空操作 → 新增 `resume_task()` 重跑转录，宿主在锁外调用；
    空闲时也显示一个无效的 挂起 按钮 → 只在 `turn_running || suspended` 渲染。挂起中发消息改为
    带引导续跑（`continue_task`），不再开新轮丢掉停住时的工具结果。
  - **模型选择器**：打开时切到当前模型所在供应商（`model_picker_focus`），而不是上次浏览的那页。
  - **页面切换滚动（第三轮）**：前两版都败在"在后面的 tick 里设 scrollTop"（钳位成 0 / 先画顶部再跳）。
    现在恢复目标在**同一个 render 里**应用：patch 之后先读 `scroll_height()/client_height()` 强制布局，
    再 `set_scroll_top()`，第一次绘制就是对的；`switch_tab` 也走这条路径。
  - 388 用例 3642 断言全过；web / webview 双变体编译通过。内置浏览器面板这次起不来
    （guest not attached），改用 HTTP 断言（服务端 CSS/wasm 含新类、无旧 `thinking-sel`）+ NullDom 用例。
    详见 current.md <86>。

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
