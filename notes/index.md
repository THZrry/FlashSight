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
  - file.ls 分基础/详细两种模式：基础只给文件名（默认，省 token），详细才带日期/大小/CSV。
  - regex 从 C:/aaa/WhaleJS 的 qjs 实现移植：libregexp.c + libunicode.c + unicode_gen.c
    + cutils.h/libunicode.h/libunicode-table.h/unicode_gen_def.h；三个回调
    lre_check_stack_overflow/lre_check_timeout/lre_realloc 由宿主提供（quickjs.c 有参考）。
    移植需标注来源（MIT，Fabrice Bellard）。
  - 工具 skill 描述必须简明清晰、中英双语，用设置键切换思考语言。

<16>(PLANNED): addon2 后端增强实现计划（分四阶段，纯后端）：
  A. 工具补全：A1 工具真执行（Loop 批准后真调+结果回填+循环到无 tool_calls）；
     A2 check_dir（子代理筛 ls）；A3 file.regex（qjs 移植）；A4 rm/写接权限体系；
     A5 噪声检测（write/diffWrite 前，unicode 异区+引号不平衡）；A6 file.ls 基础/详细。
  B. 健壮性：B1 中断-继续（保存 turn 上下文+session.continue）；B2 进程 todo 工具
     （含子代理独立 todo）；B3 计划评价 revise 流（feedback 注入下一轮）。
  C. 工作流：C1 WorkflowStore；C2 WorkflowRunner+图数据；C3 步骤重试/跳过/定向引导。
  D. 环境：D1 lsp 工具；D2 浏览器 skill 加载口。
  顺序 A->B->C->D，每阶段补单测 test_addon2.cpp，全量回归保 390+。

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
  network.fetch 接线（NetClient）；vision 改为明确 not-supported（需平台后端）。详见 current.md <21>。

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

