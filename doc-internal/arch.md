# FlashSight 架构（arch）

> 上游决策：`rules/goal.md`、`rules/user.md`、`rules/addons.md`（定稿）、`rules/QA.md`（决策记录）。
> 本文档是实现起点，其他 doc-internal 文档细化各子系统。

## 1. 分层

```
UI 层（fs_ui，独立 target，见 §4）
  │ FsApi 调用（现同进程函数调用，以后整体换 HTTP）+ 事件推送接收
  ▼
App/Core 层（fs_core）
  ProjectManager / SessionManager / SettingsStore / UsageStore
  PlanStore / TerminalRegistry / LockArbiter
  ▼
Agent 引擎（fs_core）
  Loop / Planner / Suspender / WakeScheduler / HistoryBuilder / OverfitDetector
  ▼
工具层（fs_core，内置 MCP）
  file / terminal / network / vision / subagent / ask
  ※ openWith 是 GUI 功能，不在工具层，模型不可调用
  ▼
Provider 层（fs_core）
  OpenAI Chat / OpenAI Response / Anthropic Messages / 自定义 endpoint+header / 套餐
  ▼
存储层
  ~/.flashsight/（全局） + <project>/.flashsight/（项目级），详见 session-history.md
```

- UI 不存业务状态，只渲染 + 转发事件；后端变化经事件推送（见 ui-events.md）到 UI 做 diff 更新。
- 与 OpenCode 对齐功能面，不做线协议兼容；hooks 等未定语义记 TBD，不阻塞 v1。

## 2. 源码布局（建议，搭架子时落）

```
src/
  core/       # App/Core：project/session/settings/usage/plan/terminal-registry/lock
  agent/      # 引擎：loop/planner/suspender/wake/history/overfit
  tools/      # 内置 MCP 各工具实现
  provider/   # 各供应商 client + 模型表 + quota
  storage/    # 路径解析、full.json/runtim.json 读写、JSONL、patch 栈
  common/     # 日志、错误码、时钟（可注入，方便单测）、i18n
  ui/         # fs_ui 独立目录：页面 + 事件订阅 + DOM diff（WhaleUI 就绪前只搭架子）
  main.cpp
tests/        # fs_tests：按 FsApi 表写单测
3rdparty/     # ghsl.filesystem、nlohmann/json、httplib、md4c(fork)、dtl 等
```

## 3. xmake targets

| target | 类型 | 内容 | 说明 |
|---|---|---|---|
| `fs_core` | 静态库 | core+agent+tools+provider+storage+common | 桌面版与 wasm 版共用 |
| `fs_ui` | 静态库 | src/ui | 独立目录、独立 target；WhaleUI 未成型前只留空壳 + 事件类型定义 |
| `flashsight` | 二进制 | main.cpp 链接 fs_core + fs_ui | 桌面主程序 |
| `fs_tests` | 二进制 | tests/ 链接 fs_core（+fs_ui 桩） | 单测 |

wasm 版以后 = `fs_core`（Emscripten 编译）+ 浏览器前端，跑在本机做远程访问，不另设存储方案。

## 4. UI 独立说明

- UI 层单独文件夹 `src/ui/` + 单独 target `fs_ui`，与 `fs_core` 之间只允许经 FsApi + 事件表交互，
  禁止直读后端内存。此隔离是以后拆 wasm 版的前提。
- WhaleUI API 未完善：v1 只定“画什么、推什么”（见 ui-events.md），不定具体控件 API；
  UI roadmap 排在最后（见 roadmap/README.md）。

## 5. FsApi 方法表（v1_pow，动工前定死，单测按此写）

```
project.*   : list / open(path) / create(path) / remove(pid) / get(pid)
session.*   : new(pid,cwd) / open(sid) / close(sid) / fork(sid,turnId) / rollback(sid,turnId)
              forget(sid,scope) / send(sid,text,attachments) / append(sid,text)
              suspend(sid) / resume(sid) / cancelWake(sid)
              setModel / setThinking / setPermission / setAutoMode / setEnterSend
              refreshConfig(sid)
tool.*      : read_file/ls/find/write/edit/rm
              terminal_spawn/exec/kill/poll
              fetch_url/search
              screenshot/readImage
              ask.answer(sid,askId,selection)   # 用户侧回答；自动模式不下发 ask 工具
              subagent_spawn/cancel/list
terminal.*  : list(sid) / show(tid) / kill(tid) / setEncoding(tid,enc)
usage.*     : query(filter) / exportCsv(filter,path)
settings.*  : get/set（全局+项目级，带来源标注） / detectShells() / setShell(name)
provider.*  : listProviders / listModels(providerId) / testConnection(providerId)
mcp.*       : import(path) / enable(name) / disable(name) / list()
plan.*      : get(sid) / update(sid,planTree)
```

v1 不做 `plugin.*`（插件页灰色禁用）。

## 6. 全局约定

- C++14；无 `std::filesystem`，用 ghsl.filesystem（以后无缝换 std）。
- 时钟、随机数、网络必须可注入/可桩，单测不碰真网、不碰真时钟。
- 错误码统一 `common/` 错误类型，拒绝类错误带机器可读原因（给模型换道用）。
- 日志分级，终端输出与模型流式输出走事件，不走日志。
