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

## 当前状态（更新 2026-10-13）

- P0-P3 + 缺口补齐 + addon2 后端增强 + addon3 UI 框架重做 + 终端/网络接线全部完成。
- UI 四层重构，构建纯 xmake（无 bat/脚本），事件端到端已验证；86 用例 578 断言通过。
- 等：WhaleUI 纯 C API（接 WhaleDom）、vision 平台截图/图像解码后端。
