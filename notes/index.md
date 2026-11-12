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


<124>(FIXED): 终端数量/超时、短视图持久化、空自动轮历史、引导归属、provider 串线、规划直开、todo 规则。
  - **终端列表/计数**：`terminal_list` 和 `terminalCount` 现在同一口径：只列/只数 **user 终端和带 label 的 model 终端**；未标记的 model-fg/bg 记录（891 条里绝大多数）不再进入终端列表，也不再让会话圆点显示“十几个”。标签、概览、计数一致。
  - **terminal timeout**：`timeoutMs` 确实是毫秒，之前超时分支是 `promote_to_background`，所以命令表面“超时”却继续跑半小时。现在前台超时会 terminate→2s→kill，drain 输出、`mark_exited`、返回 `exited/timedOut/exitCode=-1`；新增 `terminal_foreground_timeout_kills`。后台命令仍用 background 显式启动。
  - **短视图一直触发**：`AgentLoop` 读 compaction 时把缺省 `mode` 设成了 `"full"`，导致“没有全量摘要时短视图也要落盘”的守卫条件 `prior_mode != "full"` 永远为假——第一次短视图从来没写过 `compaction.json`，下一次又从头重放完整历史。默认改成 `""`；新增 `loop_persists_first_short_view`。现在首個短视图会留下 `{summary,compactedTurns,lastTurnId,mode}`，下一轮按摘要+未折叠轮次构建。
  - **空自动轮污染历史**：旧会话里存着几十条 `SYSTEM: 继续` + 空 assistant 回合。`HistoryBuilder` 的 token 估算、fold、正常 replay 现在都跳过 `autoRound>0` 的 SYSTEM nudge 和空 assistant；有内容的自动轮保留。新增 `empty_auto_rounds_are_not_replayed`。
  - **引导（steering）**：UI 在一轮运行时发送不再乐观追加 user+assistant 气泡、不再调 `session.send`；改为 `session.append` 进入当前 loop 的引导队列，并在下一次模型调用前以 user 消息注入（工具轮之间/最终回答之后都可再调一次），同时 toast“下一次调用带上”。guidance 写入 assistant turn 的 `guidance`，历史重放会作为 user 消息重新出现。这样旧任务不会被误判结束，terminal/tool 不会跑到新任务的工作过程里；新增 `loop_steering_is_applied_to_the_next_call`。
  - **provider 503 串线**：标题生成请求没有带 `req.provider`，按模型名路由到了第一个匹配连接（HappyAPI 的 selfdeploy...），但主回合仍用 A6；报错来自标题请求，所以会话没停。现在标题请求带 session provider（摘要模型被显式覆盖时才按名字解析）；subagent 继承 `ContextInput.provider`，`AgentLoop::Options.provider` 一起传递。
  - **规划直开**：`plan.get` 在打开会话/plan.update 时读取；工作记录头增加“规划”按钮，点击用 `open_file_preview(".flashsight/plans/<sid>.md")` 直接打开文档。
  - **todo 规则**：全局规则从“3 步以上才建”改成“需要动手或超过一次工具调用就建，尽量让清单一直存在”；运行时块每轮在清单为空时追加提醒，纯问答才可跳过。
  - **测试/构建**：459 用例 4300 断言全过；web / webview / whaleui 三变体编译通过。


<125>(FIXED/AUDIT): 自动轮“拿不准就换活”/auto_stop、auto 关闭即时生效、引导队列 UI、多规划文档、provider 路由、xmake 独立开关与 build_all。
  - **自动轮不让一个未知点停掉整轮**：全局规则新增“拿不准/被阻塞的步骤写进 plan/todo，先做不依赖它的工作；只有确实没有安全下一步才用 auto_stop”。运行时规则每轮也提醒建 todo。新增 `auto_stop` 工具（模型调用，非平常使用）：只结束后续自动续跑，当前回合正常收尾；subagent 工具集不暴露它。`empty_auto_streak`（连续 2 个无工具自动轮停止）继续作为兜底。
  - **关掉自动却继续**：`session.setAutoMode` 以前只写 sessionSettings，没有更新已经存在的 AgentLoop；现在同步 `AgentHost::set_session_auto_mode`，运行中的回合结束时会看到关闭状态。
  - **引导队列 UI**：发送在一轮运行时进入 host 的 `SteeringQueue`，不再产生新气泡；输入框上方显示队列，条目可“发送”（转 immediate，下次调用直接带上）、编辑、删除；后端 loop 在每次模型调用前 drain immediate+round-end，host queue 与 UI 共用同一份数据。`session.append` 现在写 host queue；guidance 仍持久化并在历史重放中作为 user 消息。
  - **多规划文档**：规划改成会话目录 `sessions/<sid>/plans/plan-NNN.md`，头部记录 `status`、`seq`、`turn`（当前轮开始时间）；`plan_update(new=true)` 开下一篇，旧文档保留；`plan_get` 读编号最大的一篇并返回 `plans` 列表。当前同一篇可继续 status/content 更新；reject 会清掉该会话全部规划。工作记录头“规划”按钮打开当前篇。
  - **provider 503**：标题生成请求带 session provider（摘要模型显式覆盖才按名字解析）；subagent 继承 `ContextInput.provider`，不再让标题/子代理按名称落到 HappyAPI 的 selfdeploy 模型。
  - **WhaleJS 卡住**：日志显示该会话 live snapshot `running=true`、`auto_round=3`、13 个工具，provider 是 A6API；usage 里实际 prompt ~180k、cache ~179k，不是低上下文。当前可复现线索不足（可能是 API 长等待/终端工具），但本次已把前台 terminal 超时改成真正 kill、自动轮兜底和 markdown 死循环修好；需要新构建复测。
  - **上下文审计**：usage 行显示 in≈180,892 / cache≈179,456（A6/deepseek-v4.1-flash），说明实际发送并未“异常少”；水位低是因为窗口大（1M 量级），不是提前压缩。历史重放只过滤空自动轮，真实 user/assistant/tool 内容仍保留；新增 `empty_auto_rounds_are_not_replayed` 和 first-short-view 持久化用例。
  - **xmake**：新增 `--web/--webview/--whaleui` 独立开关；单变体仍由 `--variant` 选择（一个 target 不能同时链接两个 host），多选编译用 `scripts/build_all.py`（项目级 xmake.lua 不能注册 task，之前的 `xmake build-all` 已移除）；文档更新。
  - **测试/构建**：460 用例 4313 断言全过；web / webview / whaleui 三变体编译通过。


<126>(FIXUP/AUDIT): 补上一轮漏掉的新反馈：无 todo 时自动轮终止、旧 todo 清理、xmake 多编译的真实边界。
  - **自动轮没有 todo/plan 也继续**：以前 `should_auto_continue` 只看 todo/plan，模型不建清单时自动轮直接结束。现在 AUTO 模式下，一个**实际执行过工具**的回合会继续（`turn_had_tools`，仅主 loop、非 unattended）；下一轮如果只有文本/无工具，空轮守卫停止；不会把纯问答无限续。新增 `loop_auto_continues_after_tool_work_without_todo`。
  - **旧 todo 清理规则**：全局规则新增“开始新任务前先 `todo_clear` 或 `todo_erase` 过时项；只有同一任务延续才复用旧清单”，避免模型把上一任务的完成项留在新清单里。
  - **xmake 的真实边界**：`xmake f` 已新增 `--web/--webview/--whaleui` 三个独立开关，但单个 target 图只能链接一个 host（web exe 和 WebView 窗口是不同二进制）。项目级 `xmake.lua` 不能注册 task，`xmake build-all` 已删除；多选编译用 `scripts/build_all.py --web --webview [--whaleui]`，逐个 configure/build 到同一输出目录。未把它伪装成真正的三 target 同时链接。
  - **WhaleJS 卡住**：现有证据是 `running=true`、`auto_round=3`、13 个工具、provider=A6API，usage `in≈180k/cache≈179k`；没有可复现的阻塞栈。已排除“上下文异常少”；本次已修前台 terminal 真正超时 kill、自动轮空转/无 todo 继续和 markdown 死循环，需用新构建复测。若仍卡，`[res]` 会显示 `turn_running`/`auto_round`/threads/终端状态，可直接区分 API 等待、终端子进程或 UI。
  - **上下文审计**：`[usage]` 的实际 prompt 约 180k（cache 179k），不是漏发/提前压缩；历史过滤只跳过空自动轮，真实 user/assistant/tool 内容保留（`empty_auto_rounds_are_not_replayed`）。水位低是窗口大，不是信息被吞。
  - **测试/构建**：461 用例 4317 断言全过；web / webview / whaleui 三变体编译通过。


<127>(ROOT CAUSE/FIX): 逐条重审上一轮反馈；修掉此前误当成“已解决”的新问题。
  - **终端“十几个点进去变几个”**：真因不是列表过滤，而是 `terminal.output` 事件不带 kind。UI 在第一条输出时把每个 model `terminal_exec` 都注册成 TermItem、塞进 `term_order`（于是有编号），会话圆点计数也把它算进去；下一次 `terminal.list` 按“user 或带 label 的 model 终端”过滤，幽灵记录消失，于是计数/编号与列表对不上。现在 tick 事件带 `kind`/`label`；UI 只注册可打开的终端，旧的幽灵记录在收到同一 tid 的输出时移除；活动会话计数也只数 user/labelled。新增 `uiapp_terminal_output_ghost_is_not_a_terminal`。
  - **timeoutMs 30 分钟不生效**：前台超时已经改为 terminate→2s→kill；但**后台终端根本没有 waiter**，`timeoutMs` 只在 `reproc_wait` 分支读取，`terminal_spawn` 的 60000ms 被完全忽略。现在 Proc 保存 deadline，`tick()` 到期先 terminate、1.5s 后 kill，全程非阻塞；省略 timeoutMs=不限（长构建不会被默认值杀掉）。工具描述同步改为“到时终止，不转后台”。新增 `terminal_background_timeout_kills`。
  - **短视图误判（关键）**：窗口判断用的是**原始 turn JSON** token 数，而实际重放会把 tool `detail`（完整命令输出/编辑 before-after）投影成 2400 字节以内。一个 180k token 的会话在原始 JSON 里可能几十万，窗口没满也会触发短视图，把早期历史换成摘要。现在 `raw_tokens_all`、`effective_tokens`、短视图保留轮的收缩全部用 `replay_tokens()`（真正送出的中性消息）计算；`keepTurns`（默认 8）先保留，只有“摘要 + 最近轮”仍超窗口时才逐轮多折一条。新增 `short_view_uses_projected_history_not_raw_turn_json`。
  - **上下文窗口按错 provider 取**：`deps.context_limit(model)` 遍历所有供应商，返回第一个同名模型的窗口。A6API 的 deepseek-v4.1-flash（1M）可能读到另一家同名条目的 128k，水位和短视图一起错。签名改成 `(provider, model)`；会话 provider 精确优先，provider 已选但模型未知时返回 0（不借别家的窗口）。标题请求也优先用 session provider，摘要模型被覆盖时才按“实际提供该模型的连接”解析。
  - **关自动仍续跑**：`stop_code=="round-limit"` 分支直接 `auto_continue=true`，绕过了 `auto_mode`。之前只修了 `should_auto_continue`，所以工具轮达到上限时仍会继续。现在该分支要求 `auto_mode && !unattended`；新增 `loop_round_limit_does_not_auto_continue_in_manual_mode`。
  - **引导不是双模式**：`AgentLoop::send` 在运行中把文本压进内部 `steering_queue_`，但 host 装了 `drain_steering` 后 `apply_steering` 只读 host queue，内部队列永远不 drain——引导静默消失。现在两处都 drain。更根本的是“发送还是引导”改由后端在真实 loop 状态上决定：`AgentHost::send` 发现回合在跑就 `steering_push(round-end)` 并返回 `{queued:true}`；UI 不再用可能过期的 `st_.turn_running` 预判，先显示乐观气泡，收到 queued 后把 user+空的 assistant 气泡都撤回、刷新队列并提示。这样旧任务不会被误判结束，terminal/tool 不会写进“新任务”，切页也不会恢复错的任务。`turn.started`/`auto.next` 负责保证等待气泡存在。
  - **provider 503 与“同时发两家”**：503 是标题请求（后台独立调用）缺 provider，按名称落到 HappyAPI 的 `selfdeploy/...`，主回合仍走 A6，所以报错但没停。现在标题请求带 session provider；主回合/子代理/标题每次只发一个连接，不存在同一请求并发发两家。残留：真正的多供应商并发只出现在余额探测（设计如此）。
  - **WhaleJS 一直卡住**：现有 `runtime.json` 是 `running=true`、`autoRound=0`、13 个工具、最后一条 usage ~180k，说明不是上下文小；更像 provider 长等待/心跳把回合挂住（旧 HTTP timeout 是 per-read，不是总时长）。新增模型调用**无进展看门狗**：10 分钟没有任何 delta 就通过 `req.cancel` 中止，`ProviderRuntime` 对取消不再重试，回合成失败而不是永远 running。这不是已复现根因，只是把“半小时间一直 running”封住；新构建仍需在 WhaleJS 会话复测。若复现，`[res]` + usage 可区分 API 等待/终端/UI。
  - **todo 规则矛盾**：旧规则一边说“任务小就不建清单”，一边说“尽量保持清单”。删掉前者：只有纯问答可以没有清单，只要动手就先 `todo_add`；开始新任务先清旧清单。
  - **xmake 多编译边界**：`--web/--webview/--whaleui` 三个开关在 `xmake f` 里可见，但单个 target 图只能链接一个 host；`scripts/build_all.py` 逐变体 configure/build 到带变体名的产物。本次已用它实际构建 web/webview/whaleui 三个 exe，但这不是“一个 xmake 调用同时编三个 target”，不伪装成那样。
  - **仍未闭环/诚实边界**：多规划只有“当前篇”直开按钮，`plans[]` 列表在后端可用但 UI 没有历史列表；短视图修复是单元测试+代码路径，未在真实长会话端到端复测；引导队列 UI 未在原生窗口手测；WhaleJS 卡住未复现出新栈，看门狗是缓解不是根因证明。
  - **测试/构建**：465 用例 4328 断言全过；web / webview / whaleui 三变体实际编译通过（`build_all.py`）。


<128>(ROOT CAUSE/FIX): 取消“保留最近 N 轮”的截断式短视图；改为全历史消息压缩；修复 81% 不压缩、自动轮压缩、中止延迟。
  - **首先认错**：上一版把 short-view 的 `keepTurns` 默认设成 8，并把它当成窗口不足时的常规路径。这不是“压缩”，是丢弃最近 8 轮以外的消息，破坏记录也破坏前缀缓存。已删除该策略：`keep_turns` 现在只保留字段兼容，值被忽略（默认改 0）；压缩时**每一轮都折进摘要，不保留任何原文后缀**。
  - **两套窗口**：`full.json` / turn shards 仍然是完整记录；`HistoryBuilder` 只构建“模型请求上下文”。压缩不再截断消息，而是把全部轮次写成逐轮 digest（每轮一小段，按轮数分配预算，保证**最新轮也在**）。只有“摘要 + system/tools”仍超过窗口时才进入 short-view fallback，fallback 只是进一步缩小摘要预算，不是丢消息。
  - **81% 不触发压缩（真因）**：`UsageStore::query` 把最后一行（失败的流，没有 `in/out`）当成 `sum.last`，于是 `last_prompt_tokens=0`，下一次 `should_compress` 永远看不到 81%。UI 的水位来自上一次有 token 的请求，所以显示 81%，压缩却不发生。现在只有 `in>0 || out>0` 的行才能成为 last；新增 `usage_last_skips_rows_without_tokens`。81% 是简单除法（`last_in/context_limit`），没有额外门。
  - **自动轮中途达到水位**：用户回合只在 `run_turn` 开始检查一次；长工具链在回合中途把水位推到 80%+ 时，下一次 auto round 仍带着旧的全量 messages 发出去。现在 auto 续跑前检查水位；达到阈值就从刚持久化的完整历史重建压缩请求（system + 全历史摘要 + 本轮回答 + 继续 nudge），保存 `compaction.json` 并发出 `context.compressed`。新增 `loop_compacts_between_auto_rounds_at_water_level`。
  - **压缩提示**：`context.compressed` 的 toast 改为明确说“全部历史 → 摘要，完整记录仍在会话里，未截断消息”；上下文百分比旁边新增“摘要”badge（tooltip 说明 request context 与完整记录的区别）。`session.loopState` 返回 `compacted`，重开旧会话也显示。
  - **拿不准的步骤**：旧规则让模型把不确定/有风险的步骤写成 todo 继续做，等于鼓励它执行风险行为。改成：**不要写成可执行 todo，不要猜**；把问题/原因/选项留在当前上下文和工作记录里，先做安全且独立的步骤，本轮最终回复明确报告未决点。只有完全没有安全下一步时才 `auto_stop`。
  - **中止延迟**：按钮以前只设 `abort_requested_`，而 cancel 只在 SSE `on_chunk` 里检查；provider 没有数据时读阻塞到 HTTP read timeout，UI 要等几分钟才显示结束。现在两层：
    1. UI 点击立即把当前 assistant 标记结束（streaming=false、ended_at）、`turn_running=false`、显示“正在停止…”，不再等后端回包；收到 `loop.interrupted/suspended` 后再清 `stop_pending`。“继续”在中止完成前不出现，避免和 abort 赛跑。
    2. `HttpRequest` 增加 `cancel` 回调；`HttplibNetClient` 起一个 50ms 轮询的 watcher，取消时直接 `Client::stop()` 关闭 socket。`ProviderRuntime` 对取消不再重试，所以中止是“标记结束 + 断连接”，不需要等流返回。
  - **仍未完成/边界**：webview 变体因用户正在运行 `flashsight-webview.exe`（PID 9480）导致输出文件被占用，`build_all.py` 链接失败；已在 `build_verify` 目录完成同一份代码的 webview 链接，并把新二进制放到 `temp/flashsight-webview-new.exe`。关闭正在运行的窗口后重跑 `build_all.py` 即可覆盖正式产物。中止的 socket watcher 没有自动化网络测试（单测不碰真网），只做了编译与路径审查；需要在真实模型流上复测“点中止→立即停”。
  - **测试/构建**：467 用例 4344 断言全过；web / whaleui 实际编译通过；webview 在备用 builddir 编译链接通过。


<129>(ROOT CAUSE/FIX): 每次调用后判断；模型总结代替逐轮预算；短视图丢工具保留每轮输入/回复；引导队列/立即单条；中止去掉继续；不再向模型报上下文占用。
  - **每次调用后判断**：`run_loop_impl` 在工具轮之间的下一次调用前检查 `last_in_tokens / context_limit`；超过阈值就把当前内存 messages 交给模型总结（`compact_in_memory`），替换成 system + 全历史摘要，并置 `compact_pending_` 使下一次用户轮强制压缩磁盘历史。回合结束时（没有 auto 续跑）再调用 `compress_durable_history`：从刚持久化的完整历史生成摘要、写 `compaction.json`、发 `context.compressed`，不用等下一次手动发送。auto 续跑边界也走同一个 helper。
  - **不再按轮给预算**：压缩摘要由模型生成。`HistoryBuilder::Options::model_summarizer` 接收完整 material（上一条摘要 + 每一轮 user 原文 + assistant 回复 + 工具名/摘要），prompt 明确要求保留：用户长期要求/约束/偏好、当前和近期需求、上一摘要里的未决问题、关键决定/文件/命令/结果。生产路径由 `AgentLoop::summarize_session` 调模型（独立的 `sid#summary` 用量桶，不污染水位）；只有单元测试/嵌入调用没有 summarizer 时才用确定性 digest fallback。
  - **短视图改为丢工具调用**：窗口连摘要都放不下时，`short_view_messages` 保留 system + 每一条 user 输入 + 每一条 assistant 回复，丢掉全部 toolCalls/tool 结果，不按轮截断预算。短视图的 `summary` 也保存这些 U/A 行，重开可复用。
  - **引导队列新语义**：队列里的全部条目在下一次模型调用一次性带上（`drain_all`）；点“立即”只 `take` 这一条，打断当前调用，等 loop park 后用 `session.continue(suppressQueue=true)` 只发送这一条，其余队列保留到后续调用。`AgentHost::send` 在 parked 时会把队列 + 输入一起续跑，所以挂起期间写的引导也会在继续时发送。
  - **引导显示**：`ChatMsg::guidance` + `work-guidance` 独立块，工作过程里用户引导和模型 narration/tool 分开；stored turn 的 `guidance` 重开也恢复。运行中 composer 增加“引导”按钮（`comment-dots`，与正常 `paper-plane` 发送不同图标）；正常的发送在运行中仍然按队列处理。
  - **去掉继续按钮**：中止后不再显示“任务已中断 [继续][忽略]”条；仅保留短暂的“正在停止…”，随后正常发送（或队列续跑）即可。`stop_pending` 期间不显示继续，避免和 abort 赛跑。
  - **不向模型报上下文占用**：`build_runtime` 删除 context% / tokens 行。模型无法利用这个数字（压缩由宿主自动做），UI 仍显示给用户。
  - **仍未验证**：中止的 socket watcher 仍无真实网络单测；引导“立即”打断→单条续跑、压缩后的模型摘要、工作过程引导块都需要在新构建里手测。webview 正式产物仍被正在运行的 `flashsight-webview.exe` 占用，最新二进制在 `temp/flashsight-webview-new.exe`；关闭窗口后跑 `build_all.py` 覆盖。
  - **测试/构建**：470 用例 4359 断言全过；web / whaleui 最新代码编译通过；webview 在备用 builddir 编译通过。


<130>(FIX): 引导按钮替换规则/图标；WhaleJS 无进展看门狗；后台事件不再重绘当前页；旧历史渲染路径复核。
  - **引导按钮**：运行中且输入框/附件非空时，主按钮替换成“引导”，图标是 `comments`（与正常 `paper-plane` 不同）；输入框和附件都为空时才显示“中止”。输入框只在 empty↔non-empty 边界触发一次 render，不会每敲一个字重绘。
  - **WhaleJS 01:54 卡住证据**：`runtime.json` 最后工具是 1676（01:48），之后 usage 再也没有记录；`running=true`，autoRound=0，模型调用没有产生任何 token。不是终端进程还在跑，而是**终端结束后的下一次模型调用挂住**（旧 23:19 二进制没有看门狗）。新增 `Options::model_progress_timeout_ms`（默认 10 分钟），`req.cancel` 超时返回 true；`HttplibNetClient` 的 CancelWatch 调 `Client::stop()` 断 socket，`ProviderRuntime` 对取消不重试。新增 `loop_aborts_a_model_call_with_no_progress`（用 30ms 预算验证）。旧进程不会获益，必须用新二进制；正式 webview 仍被运行中的旧实例占用，最新在 `temp\flashsight-webview-new.exe`。
  - **后台会话事件不再重绘当前页**：以前背景会话的 `stream.delta/tool.start/tool.update/assistant.interim/loop.usage/todo.updated/terminal.output` 每个事件都 `render()` 当前页；两个会话并行时，另一个会话 300+ 工具事件会让可见页面反复 build/diff，表现成“无响应”。现在这些 hot 事件只更新该会话的 stashed view，不再重绘当前页；turn/loop/auto/subagent 等生命周期事件仍立即渲染，tab/dot 不延迟。
  - **旧历史 markdown**：复核了路径——`msg_nodes_` 以 (uid,rev,sig) 命中时直接复用同一 VNode，`tool_block_cached` 对每个工具行单独 memo，`markdown_blocks` 只在该消息 rev/sig 变化时重建；固化的旧消息不会在每次 render 重新解析 markdown。真正的重复开销来自上面的后台事件重绘和 streaming 消息的每秒计时。没有再加历史截断或渲染窗口。
  - **仍未验证**：按钮切换、后台不重绘、看门狗都需要在新 webview 手测；`dom_slots` 在旧进程里每 30 秒 +1 的小泄漏仍未处理（不是本次卡死原因）。
  - **测试/构建**：471 用例 4361 断言全过；web / whaleui 最新代码编译通过；webview 在备用 builddir 编译通过。


<131>(FIX/ANALYSIS): 三个变体改为真正的 xmake target；推理流不再被忽略（长请求假死的直接原因）；缓存异常分析。
  - **xmake 三 target**：删除 `scripts/build_all.py` 和 `--variant/--web/--webview/--whaleui` 配置。现在图里就是 `flashsight-web`、`flashsight-webview`、`flashsight-whaleui` 三个可执行 target，外加 `fs_ui_host_webview`/`fs_ui_host_whaleui` 两个宿主静态库；共享 `fs_ui` 只编一次。一条命令 `xmake b flashsight-web flashsight-webview flashsight-whaleui` 在备用目录实测三个 exe 全部链接成功（webview 正式产物被运行中的旧实例占用，不能覆盖）。CI 和 `doc-internal/build.md` 改成按 target 名构建，`package.py` 的提示同步。
  - **长请求“卡住”的直接原因**：ProviderClient 只解析 `delta.content`，完全忽略 DeepSeek 的 `reasoning_content` / OpenAI 的 `reasoning`。`thinking=max` 时模型先流几十万 reasoning token，应用一个 delta 都收不到：UI 看起来完全静止，`model_activity_ms_` 也不更新，原来的看门狗会把它当无进展（甚至误杀）。usage 里那次 `latency=390177ms, out=4096, cache=0` 的调用就是这种长思考；它不是死连接。现在：
    - `ChatRequest::reasoning_delta`；OpenAI SSE 解析 `reasoning_content`/`reasoning`，累积到 `ChatResponse::thinking`；
    - `ModelCallRequest::on_reasoning` → `AgentLoop` 更新活动时间、写 `live_thinking_`、发 `thinking.delta`；UI 把内容放进 `ChatMsg::thinking` 并展开，不混进答案；
    - reasoning 也标记为 `emitted`，中途断流不会重试并重复可见内容；流结束时把 thinking 持久化到 assistant turn；
    - 无 progress 的硬看门狗仍是 10 分钟（`model_progress_timeout_ms`），现在不会被正常长思考误触发。
  - **缓存命中分析**：该会话 usage 的异常不是 80% 的单次命中，而是**少数 `cache=0` 或 `cache=256` 的 30 万 token 请求**把累计命中率拉到约 80%。例如 `in=307640/cache=0`、接着 `in=307405/cache=256`、`309246/256`、`311104/256`、`313077/256`。`256` 意味着上游只命中了开头约 256 token（系统提示），不是普通的缓存 TTL 归零；这更像**中转站把请求路由到了没有该前缀缓存的上游通道**，或该通道只缓存了系统段。应用侧检查：同一 provider/model、system/tools 顺序稳定、runtime 块只在最新 user 消息末尾；`compaction.json` 在这段时间没有更新，因此不是压缩前缀重排。暂未在应用里找到会周期性改写前缀的路径。要彻底区分“应用前缀变了”和“中转站换通道”，下一步应在每次请求记录 prompt 前缀 hash；本次没有伪造这个结论。
  - **测试/构建**：471 用例 4361 断言全过；一条 xmake 命令在 `build_verify` 下实际链接出三个 exe；正式 `flashsight-web.exe`/`flashsight-whaleui.exe` 已更新，`flashsight-webview.exe` 仍被运行中的实例占用，最新在 `temp\flashsight-webview-new.exe`。


<132>(ROOT CAUSE/FIX): WhaleJS 卡住是前台 terminal_exec 里 run.exe 永不退出且 timeout 失效；工作过程思考改为按到达位置插入、可折叠、结束自动折叠。
  - **WhaleJS 16:25 卡住的现场**：gdb attach 到 16720，线程栈停在 `TerminalManager::spawn -> reproc_wait -> WaitForSingleObjectEx`。子进程树：bash -c `gcc ... | tail; echo "=== TRANSLATED ==="; ./temp/emcctest/run.exe | tail`，`run.exe`（PID 26752）从 16:25 一直活着，日志停在 `=== TRANSLATED ===`。模型调用不是阻塞点，是 terminal_exec 命令本身没有退出。
  - **timeout 为什么没生效**：前台等待用 `timeoutMs`；模型可以传 0 表示不限，或传一个很大的值，于是 `reproc_wait` 直接无限等。现在前台命令 `timeoutMs<=0` 按 60000ms，且硬上限 10 分钟；后台 `terminal_spawn` 的 0 仍表示不限（后台有 tick 超时）。
  - **中止也要能杀**：新增 `ToolContext::cancel`，`AgentLoop` 注入 abort 标志；前台等待改成 100ms 切片轮询，cancel 后立即 terminate/kill，返回 `cancelled`，不再等满 timeout。`AgentHost` 的 terminal_spawn 回调带上 cancel。新增 `terminal_foreground_cancel_kills`。
  - **工作过程顺序（按 note <96> 的原始要求回正）**：不是“工具 → 解说 → 思考”三段，也不是把整轮 reasoning 合成一个块丢在最后。现在 `ChatMsg::ThinkBlock{text,after,open,streaming}` 按“到达时已执行工具数”插入同一条流：工具前到达的思考在工具前，工具后的思考在工具后。`thinking.delta` 追加到当前段；`stream.delta`/`tool.start`/`assistant.interim`/`turn.done` 会把当前段标成结束并自动折叠。思考块点击可展开/折叠；旧存储的单个 `thinking` 作为兼容块保留。
  - **持久化/重开**：assistant turn 存 `thoughts`；`live_state`/`attach_live_turn` 也带 `thoughts`，切页/重开还原位置和折叠态；`message_sig`/debug 统计包含 thoughts。
  - **测试/构建**：473 用例 4373 断言全过；新增思考流自动折叠/位置、前台 terminal cancel 用例。


<133>(ROOT CAUSE/FIX): 前台 terminal_exec 的预算由工具强制 60s，到时返回并转后台；终端 UI 与后端重新接上（事件丢字段/列表过滤/计数覆盖）；思考块完全复用工具折叠行。
  - **60s 是工具的预算，不是模型参数**：旧代码允许模型传 0（→60s）或最高 10min；模型传个大值就会“一分钟早过了还不返回”。现在前台 `terminal_exec` 的 wait_ms = min(模型 timeoutMs>0, 60000)，省略/0 也是 60000；再大的值也只等 60s。后台 `terminal_spawn` 的 0 仍是不限（显式 timeoutMs 由 tick 在后台执行）。
  - **到时返回，不是杀死**：60s 到达时 `reproc_wait` 返回 ETIMEDOUT，进程不再 terminate；`promote_to_background` 把它改成 model-bg（wakeOnExit=true），返回 `status:"running"`、`timedOut:true`、`budgetMs:60000` 和已产生的输出，模型自己决定 terminal_poll / terminal_kill / 继续等。用户中止仍立即 terminate→2s→kill，两个分支分开。工具结果 summary 明确写“超时（60000ms），命令已转后台（终端 #N）”，不再看起来像普通完成。
  - **为什么 UI 看不到终端（两条断链）**：
    1) `FsApi::terminal_tick` 重新构造了 `{sid,tid,text}` 事件，把 manager 已经带上的 `kind/label/timedOut` 全部丢掉，UI 只能把 model 终端当 ghost 隐藏；
    2) `terminal_list` 只显示 user 或“带 label 的 model 终端”，无标签的 `terminal_spawn`/晋升后台终端永远不出现；`terminal.output` 的 UI 分支用同一错误谓词，`live_session_row` 又用未加载的空本地表覆盖后端 terminalCount。
    现在事件 payload 原样透传；`terminal_list` 的谓词是 user / model-bg / 带 label，且必须（运行中或日志存在）；UI 输出分支同一谓词；会话圆点取 max(后端计数, 本地计数)；terminal 工具的 `tool.update` 后主动 `load_terminals`。无标签的 model-bg 在“所有终端”里可见，并新增“超时·后台 / 超时退出”状态。
  - **日志事件还有一处断链**：`tick()` 用“本次 tick 开始时 p.out 的大小”判断新输出，而前台 spawn/poll 在 tick 之前已经把输出 drain 完，导致 foreground 的 terminal.output 从不发出。改为和 `p.reported` 比较（1MB 截断时同步 reported 的旧逻辑保留）。新增 FsApi 用例断言事件带 kind，以及超时记录出现在 terminal.list 且仍 running。
  - **思考块复用工具折叠组件**：删除自定义 `think-block/think-head/think-body`，`thoughts[i]` 和旧的单个 `thinking` 都渲染成 `ToolBlock{name:"思考", detail:text, expanded:open, running:streaming, plain:true}`，走 `tool_block_cached`（流式时不缓存，否则每个 token 的完整 growing text 都会进 4000 条 row cache）；点 `tb-head` 发 `think:<idx>`/`legacy-think`，与工具行完全同一套折叠。`plain` 关掉 JSON 高亮，`tb-prose` 让正文换行而不是横向滚动。
  - **为什么整组不显示**：`chat_message` 的 `has_work` 只看 thinking/tools/interim/work_live，漏了 `thoughts`；纯 reasoning 的第一轮会整块不渲染。已加入 `!m.thoughts.empty()`。旧存储的 thinking 现在默认折叠（`thinking_open` 默认 false），点开可见；测试同步改成点开后再断言文本。
  - **测试/构建**：480 用例 4419 断言全过；新增 `terminal_foreground_timeout_detaches`、`terminal_foreground_budget_is_capped`、`terminal_registry_timed_out_roundtrip`、`uiapi_terminal_output_event_carries_kind`、`uiapi_terminal_timeout_is_listed_and_running`、`uiapp_thinking_row_uses_tool_fold`、`uiapp_terminal_output_background_model_terminal_is_visible`、`uiapp_backend_terminal_count_survives_empty_live_list`。`xmake b flashsight-web flashsight-webview flashsight-whaleui fs_tests` 一条命令重编三变体并通过。


<134>(ROOT CAUSE/FIX): 两个会话同时工作时的假死不是模型/上下文，而是终端 poller 在 `reproc_wait` 里持锁永久阻塞，随后全局 UI 锁与终端锁交叉锁死；同时修正思考/解说顺序、思考图标与空思考、引导乐观气泡和队列上界。
  - **现场（gdb 21684，2026-11-10 01:21）**：线程 2（终端 poller）在 `TerminalManager::tick -> reproc_wait -> process_wait -> WaitForSingleObjectEx`；线程 8（`terminal.grid`）持有 UiApi 全局锁等终端 `mu_`；线程 9（WhaleJS 的 `terminal_exec`）和线程 5（WhaleUI 的 `terminal_poll`）都在等 `mu_`；线程 6/7/10/12/13 都在等 UiApi 全局锁；主线程在 `~NativeBackend` join 这些 worker。两个会话共享的正是这一条链：poller → `mu_`，UI 终端路由 → 全局锁 → `mu_`。
  - **为什么 60s 兜底没生效**：`reproc_wait(handle, 0)` 在 Windows 上不是非阻塞检查。它的 `pipe_poll` 把 exit pipe 可读当成“进程已退出”，随后 `process_wait` 调 `WaitForSingleObject(INFINITE)`；BusyBox 起子进程（这次是 `run.exe`）时会提前关闭继承的 exit pipe 句柄，于是 pipe 可读但 shell 还活着，等待永不返回。前台 60s 切片、tick、poll、kill 全踩同一个坑；取消/看门狗也救不了，因为 turn 线程卡在获取 `mu_`，根本没进入模型调用。
  - **修复**：`Proc` 自己保存 `pid` + `OpenProcess(SYNCHRONIZE|QUERY_LIMITED_INFORMATION)` 句柄（POSIX 用 `waitpid(WNOHANG)`），`try_wait` 真正非阻塞；`spawn/poll/tick/kill` 不再调用 `reproc_wait`。kill 在锁内 terminate，释放 `mu_` 后再等退出（2s+2s），不会把其他会话按在锁上；析构也不再使用 NULL stop actions（以前只是关管道，子进程会变孤儿），改为 terminate+kill。新增 `terminal_foreground_timeout_with_a_shell_child`（`sleep 30 & wait`）复现 BusyBox 子进程形态，并断言 tick 在 1s 内返回。
  - **共享注册表竞态**：`TerminalRegistry` 没有锁，`terminal_list/session_list/terminal_show/terminal_tick/spawn` 在 worker 上直接读写 map。现在 manager 的 `mu_` 改成 recursive，新增 `entry_snapshot/entries_for_session/add_saved_entry`，所有 FsApi 读都拿锁拷副本；UI 的 `terminal.grid/poll/kill/tick` 从 UiApi 全局锁排除（它们只碰 manager 自己的锁），避免“全局锁 → 终端锁”的交叉等待。
  - **思考与正文顺序**：同一轮里 reasoning 在模型调用过程中流出，`assistant.interim` 在调用返回后才发出，两者带同一个 tool anchor；旧渲染器先 flush interim 再 flush thought，导致后到的解说排在先到的思考上面。现在同一 anchor 先思考后解说，尾部的思考也排在同轮解说之前。新增 `uiapp_thought_precedes_narration_at_same_anchor`。
  - **思考图标/空思考**：思考行改用 `fa-brain`，不再显示工具的成功勾；`text_is_blank()` 过滤只有空格/换行的 reasoning（不建行、不算 has_work、不参与未读工作块），存储 turn 和 live snapshot 读取时也跳过。新增 `uiapp_blank_thought_is_not_rendered`，`uiapp_thinking_row_uses_tool_fold` 断言没有 `tb-ico ok`。
  - **引导气泡与队列**：`dispatch_message` 在 `turn_running || auto_round>0` 时不再先画用户气泡/等待占位符再撤销，只清空输入并发送；后端回 `queued` 即只刷新队列。若 UI 状态过期而后端实际开了新轮，则由 `turn.started`（携带用户文本）在回答前插入问题气泡和占位符（文本不会丢）；发送失败恢复输入。队列硬上限 `kMaxSteeringItems=20`，满了返回错误而不是静默顶掉旧项；`.steer-queue` 增加 `max-height:min(220px,32vh)` + 滚动，不再把输入框顶出窗口。新增 `uiapp_guidance_send_draws_no_bubble`、`uiapp_guidance_race_draws_turn_from_reply`、`steering_queue_is_bounded`。
  - **测试/构建**：486 用例 4465 断言全过。主 build 的 `flashsight-web` 和 `flashsight-whaleui` 已重链；正式 `flashsight-webview.exe` 被卡死的旧实例占用，最新用独立 builddir 构建后复制到 `temp\flashsight-webview-new.exe`（该临时 builddir 已删除）。**旧进程不会获益，必须关掉重开新二进制。**


<135>(ROOT CAUSE/FIX): 压缩摘要没有进入会话正文，54% 是压缩后新长出来的 312 轮；真正的问题是“摘要”标签常驻 + 后台压缩事件未按会话隔离 + 关窗不中止 agent 导致控制台不退；引导按 tool anchor 落在工具后并截断。
  - **压缩没有漏进正文**：扫描 WhaleUI 会话 030-037 全部 turn，没有任何 assistant/user 文本包含 `[Earlier conversation was compacted]` 或 `[Key points]`；摘要只作为 `prior_summary` 拼在模型请求里。可见的“压缩漏出来”是模型行上的常驻“摘要”标签，已删除。
  - **54% 不是压缩失败**：`compaction.json` 的 savedAt=1794159114361（283 轮），之后 usage 的前几次请求是 19,184 / 19,850 / 20,165 / 20,474 tokens（1M 窗口约 2%），说明摘要确实把上下文降回清爽水平；当前 551,903 tokens / 54% 是压缩后又积累了 **312 轮**，达到 80% 阈值（`history.compressAtPct=80`）前不会再次全量压缩。边界处还看到 814k 整段请求与 20k 请求交替（814575/815371，cache≈814k），像是同一会话曾有第二个执行者/实例；本次没有加跨进程会话锁，先记录证据。
  - **水位立即反映下一次请求**：`HistoryResult` 新增 `projected_tokens`（system+tools overhead + summary），`context.compressed` 增加 `estTokens`；UI 收到后把 `context_tokens/context_pct` 直接改为下一次请求的估算值，不再停留在触发压缩的 80%/54%。`est_tokens` 保持“不含 system 的对话内容”语义，`compress_actually_shrinks_messages` 继续成立。`context.compressed` 与 `steering.applied` 移入 `apply_session_event`，后台会话的压缩/引导不再丢，也不会把 toast/标签打到当前会话上。
  - **关窗后控制台不退出**：`NativeBackend::~NativeBackend` 以前直接 join 8 个 worker，正在跑的 turn/subagent 会把主线程一直按在 join 上。现在顺序是：退订事件监听（`FsApi` 的事件表加 recursive mutex，remove 会等 in-flight emit）→ `abort_all_agents()`（`AgentHost::abort_all` 置 `shutting_down_`、interrupt 所有主 loop；子代理 loop 不在 loops_，通过新增 `Options::external_cancel` 在模型/工具/摘要请求里读到同一标志）→ 停终端 poller → 再 join worker。子代理工具现在继承 `ctx.cancel` 并重绑 `terminal_spawn` 的 cancel；标题/摘要这类 post-turn 网络调用也检查 shutdown/取消，不再拖住退出。
  - **引导位置/长度**：`apply_steering` 记录 `executed_calls_.size()`，落盘为 `guidanceAfter`，`steering.applied` 事件带 `tools`；UI 按这个 tool anchor 在已经跑过的工具**之后**渲染引导（与 thought/interim 同一到达流），不再固定放在工作过程顶部。工作日志中的引导正文截到 240 字节（完整文本在存储轮里，title 可悬停查看）；队列仍是 20 条上限 + max-height 滚动。
  - **测试/构建**：489 用例 4474 断言全过；新增 `uiapp_guidance_uses_its_tool_anchor`、`uiapp_compaction_is_session_scoped`、`loop_external_cancel_aborts_a_blocked_call`。三变体重链；webview 正式 exe 仍被运行中的旧实例占用，新二进制在 `temp\flashsight-webview-new.exe`（没有动用户正在跑的任务）。


<136>(ROOT CAUSE/FIX): WhaleJS 运行中切页上下抽搐/渲染进程崩溃是单个 auto 轮累积 2387 条工具行、每帧全量绘制导致（WebView 19GB、render 2543ms）；同时修 read_image 的图片请求体、edit_file 的 after/入参兼容、输入法/路径提示、项目卡片滚动与左栏收起、缓存单次值、todo/plan 状态与批量添加。
  - **现场（[res] + 运行数据）**：活动会话 msgs=119、tools=2387、tool_bytes=3.3MB、auto_round=46/47、dom_slots=1,416,359、webview_mb=19,296、30 个 WebView 进程、`[slow] render ms=2543`。一个 auto 轮把 47 轮的工具全堆在**同一条 streaming assistant 消息**里；work 折叠在运行中保持打开，每次 tool/stream 事件都重建/补丁全部 2387 行，布局和滚动反馈互相触发，最后渲染进程被内存压死。存储层没有问题，危险的是“把完整工具日志每帧都画出来”。
  - **修复**：`chat_message` 的工作过程只渲染最近 120 行（streaming）/300 行（已完成且用户展开），前面显示“更早的 N 步已省略；完整记录保存在会话中”；thought/interim/guidance 的 anchor 同步跳过隐藏前缀。完整工具记录仍在 turn 里，工具面板/重开可查。新增 `uiapp_huge_work_log_is_sliced`。
  - **read_image 真正进入视觉请求**：OpenAI Chat 的 `tool` 消息不能带 image content；适配器现在把 tool 结果里的图片抽成**紧随工具组之后的一条 user 消息**（`image_url` data URI），Anthropic 保留在 `tool_result` 的 image block，Responses 用 `input_image`。历史回放仍然只带 path/width/height 元数据（project_tool_result 丢掉 dataUri），不会把 base64 每轮重发。新增 `adapter_openai_tool_image_becomes_user_part`（同时验证 Anthropic）。
  - **edit_file**：结果 `changes` 不再回显 `after`（它就是调用方刚发送的 newText；read-back warning 改成提示重新读取）。入参同时接受 canonical `{edits:[{start,end,newText}]}`、flat `{path,hash,start,end,newText}` 和已知别名 `startLine/endLine/text`；JSON 对象 key 顺序无关，未知字段仍拒绝。`edit_file` 描述写明这一点。
  - **输入法/路径提示**：wasm/webview 的 input 监听在 `evt.isComposing` 时不再把半成品拼音送进应用（compositionend 的最终 input 仍会到）；keydown/keyenter 在组合中也不处理。caret 不依赖 DOM：由新旧文本的公共前缀/后缀推导。路径提示围绕 **caret 所在的 token**（不是整段最后一个 `/`），只认含 `/`/`\`、非 URL、非 `-flag` 的 token；接受时替换整个 token（含 caret 后的后缀）并用新增的 `IDom::set_text_caret` 恢复 caret，不再跳到末尾。后台 `complete_path` 支持 `./`（approve 档也允许，`../` 和绝对路径才需要 outside 权限）、Windows drive 绝对路径、以及以**项目文件夹名**开头的 token（`WhaleJS/src/` 按项目根解析，但替换保持用户拼写）。
  - **主页/左栏**：两栏项目卡片在 10 条处截断，**展开控制放在列表最后一行**（“展开剩余 N 个 ▾” / “收起 ▴”），不是标题栏开关，也不是内部滚动盒；展开后显示全部会话。左栏每个项目同样在会话列表末尾放“收起/展开（N）”，收起只隐藏该项目自己的会话，不影响其他项目。新增 i18n `收起/展开`。
  - **输入框 Enter/点击发送无反应**：现场同时满足“2387 行工作过程 + 每帧 2.5s render + WebView 19GB”，UI 线程被渲染占满/渲染进程死亡后，输入事件自然全部失效——不是监听器丢失。行数 cap 是根因修复；另外 IME 改成显式 `compositionstart/end` 状态，compositionend 主动把最终文本交给应用（有些 WebView2/IME 组合在最终 input 上仍报 isComposing=true，单靠 input 判断会吞掉中文和 Enter）。
  - **缓存命中**：`usage.session.cacheHit` 不再用整个会话的累计 cache/in（冷压缩请求会把命中率拖歪），改用**最近一次报告了 cache 的请求**；UI 的命中 tokens 也取该次。`loop.usage` 本来就是单次。
  - **todo/plan**：runtime 增加一行 `状态: todo x/y, plan: active|done|none`；有未完成项时明确“不要提前总结收尾”，全部完成提示 `todo_clear`，plan active 也不能总结。全局规则同步中英文。`todo_add` 支持一次 `tasks:[...]`；add/check/erase/clear 只返回 ids/pending/total，不再每次回传整张清单（`todo_list` 才返回全文）。
  - **测试/构建**：491 用例 4491 断言全过。`flashsight-web`/`flashsight-whaleui` 已重链；正式 `flashsight-webview.exe` 仍被正在跑的任务占用，新二进制在 `temp\flashsight-webview-new.exe`。**没有关闭用户的 FlashSight**；当前进程还是旧二进制，崩溃/输入/界面修复要等任务结束重启后生效。


<137>(ROOT CAUSE/FIX): “启动时点不了”的按钮/Enter 是 VDOM 每帧 unbind/rebind 监听器与异步 DOM 批次的竞态；主页改为整表滚动、左栏改为 10 条后收起；路径提示改为整串贪心路径区间 + caret 命中；错误会话不再显示运行中。
  - **按钮/Enter 启动丢了（结构性根因）**：`patch_events` 以前发现 handler id 变了就 `dom.off` + `dom.on`。桌面适配器的 DOM 写入走 `ExecuteScript`，是**异步**的；一次启动要连续 render（项目列表、设置、会话列表），浏览器可能还持有上一代监听器，而 C++ 这边 `retain()` 已经把它丢了：点击/输入事件打到 dead id，`fire()` 找不到 handler，于是“按了没反应”。`FS_UI_EVENT_DEBUG` 的批次日志也直接显示同一节点每帧 `unbind ... / bind ...`。现在**活下来的节点保留原监听器 id**：`EventBus::rebind(old_id, new_id)` 把新闭包放进旧 id，`patch_events` 不再对已匹配的 kind 做 off/on，新 id 从 bus 移除。DOM 监听器从创建起永久有效，无论浏览器应用到了哪一批；输入框的 input/keyenter 同样受益。新增 `vdom_surviving_node_keeps_its_listener_id`、`vdom_removed_kind_is_unbound`。重建后的 web 与隔离 webview 实测“新建”能打开新页；原生 `FS_UI_EVENT_DEBUG` 日志里“新建”节点只在创建时 bind 一次，后续 render 不再出现 unbind/bind。
  - **主页/左栏（纠正 <136> 的反向实现）**：主页两栏项目卡片**渲染全部会话**，由 `.proj-right .proj-sessions { max-height:352px; overflow-y:auto }` 直接滚动；没有“展开剩余”行。左栏才是“大于 10 个隐藏”：每个项目最多 10 行，列表末尾显示 `展开剩余 N 个 ▸` / `收起 ▾`，展开只影响该项目。1440px 宽实测：46 行全在 DOM、卡片 `scrollHeight=800/clientHeight=352`、无 more 行；左栏 10 行 + `展开剩余 14 个`，点开后 24 行 + `收起`。
  - **路径提示按位置（用户给的算法）**：新增 `src/ui/ui_path_range.{hpp,cpp}`：`parse_path_ranges()` 贪心解析整串里的路径区间（`./`、`../`、`~/`、POSIX 绝对、Windows drive、`看下./src` 这种中文粘连，以及 `src/ui` 被中文前缀粘住时从 ASCII 段起算；URL/CLI flag 不当作路径，中文标点/引号结束区间），`path_range_at()` 对 caret 做命中测试。UI 只用命中区间作为 `typed` 和替换范围，接受时替换 `[start,end)` 并把 caret 放回插入点之后。实测隔离 webview：输入 `看下./sr` → 菜单 `src/`，Tab 后 `看下./src/`、caret=8，菜单继续列出子目录。新增 `path_ranges_glued_to_cjk_prose`、`path_ranges_find_ascii_path_inside_cjk_prefix`、`path_ranges_windows_drive_and_spaces`、`path_ranges_drive_glued_to_cjk`、`path_ranges_skip_urls`、`path_ranges_stop_at_quotes_and_punctuation`、`path_ranges_multiple_and_caret_hit` 等。
  - **错误会话仍显示运行中**：三处一起收口。① `run_turn` 现在包住整个实现（`run_turn_impl`），`persist_user_turn_only`/历史构建等在任何位置抛异常都会走 `fail_turn()`，loop 落到 `kTurnDone` 并发出 `loop.interrupted`，不会再停在 `kPlanning` 让 `is_running()` 永远为真；新增 `loop_exception_in_turn_start_is_terminal`。② 发送被后端拒绝时，UI 立即撤掉自己画的空回答占位、清 `turn_running/auto_round`、在消息上写错误并刷新 loopState（否则没有 turn.done 来收尾）。③ 切回会话时 `load_session_view` 不再复活 `interrupted` 视图里的 running/auto_round；`refresh_loop_state` 的 `idle` 回包若比 `turn.started` 旧（epoch 变了）就不再生效。
  - **测试/构建**：502 用例 4454 断言全过。`flashsight-web.exe` 已重链并在无头 Edge 里验证“新建”、主页 46 行滚动、左栏 10 条+展开；`flashsight-whaleui.exe` 已重链（本 shell 里 `whaleui_app_run` 立即返回，未做 UI 断言）。正式 `flashsight-webview.exe` 仍被用户正在跑的实例占用，最新二进制在 `build\windows\x64\release\flashsight-webview-new.exe`（隔离 `--root` 下已实测“新建”、路径提示、监听器只 bind 一次）。**没有关闭用户的 FlashSight**，旧进程仍是旧代码，需重启后生效。
