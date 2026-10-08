# FlashSight 设计定稿（addons）

> 定位：`rules/goal.md` + `rules/user.md` 的补充设计，只写“缺了就会前后端对不上”的部分。
> 本文件为**定稿语气**：凡写明的都是已决策；未定量、需调研的显式标 `TODO`。
> `rules/QA.md` 为本文件的决策来源记录。
> 约束：C++14 + xmake + reproc；UI 按**不支持 JS 的 webview**规划，DOM 操作是全的；
> 高性能路线：C++ 直接操作 DOM 树 + md4c 魔改输出 diff AST + diff 更新界面；
> WhaleUI API 未完善，不依赖其细节。

---

## 1. 总体分层（前后端分离，Emscripten-ready）

```
┌─ UI 层（主页 / 会话 / 新建页 / 设置；桌面风，不用 Material）─────┐
│ 只做渲染 + 事件转发，不存业务状态；经 FsApi 调后端              │
├─ 应用层 App/Core ────────────────────────────────────────────┤
│ ProjectManager / SessionManager / SettingsStore / UsageStore  │
│ PlanStore（规划树） / TerminalRegistry / LockArbiter（文件锁） │
├─ Agent 引擎 ─────────────────────────────────────────────────┤
│ Loop（手动/自动）/ Planner / Suspender / WakeScheduler        │
│ HistoryBuilder（短视图+压缩） / OverfitDetector（默认关，只提示）│
├─ 工具层（内置 MCP：file / terminal / network / vision / subagent / ask）│
│   ※ openWith 不是工具，是 GUI 功能（右键菜单），模型不可调用  │
├─ Provider 层（OpenAI Chat / Response / Anthropic / 自定义+套餐）│
└─ 存储层（JSON 全历史 / 设置 / JSONL 用量 / patch 栈 / git CLI）─┘
```

- **FsApi 边界**：一切 UI↔后端交互走可枚举的函数调用（现同进程直接调用，
  以后整体换 HTTP/浏览器—后端调用）。方法族：`project.*`、`session.*`
  （new/open/close/fork/rollback/forget/send/append/suspend/resume、setModel/
  setThinking/setPermission/setAutoMode/refreshConfig）、`tool.*`（file 读/写/
  diff写/ls/rm/find、terminal spawn/kill/poll、fetch/search、screenshot/readImage、
  ask、subagent spawn/cancel）、`terminal.*`、`usage.*`、`settings.*`、
  `provider.*`（listModels/testConnection）、`mcp.*`（import/enable/disable）、
  `plan.*`。**v1 不做 `plugin.*`**（插件页灰色禁用，见 §9）。
- **推送模型**：后端状态变化经事件推送到 UI，UI 做 diff 更新。事件类型表见 §9。
- **与 OpenCode 对齐**：对齐功能面（会话/工具/子代理/MCP/权限/历史语义），
  不做线协议兼容。hooks 语义等 goal 未写明的记 TBD，不阻塞 v1。
- **wasm 说明**：wasm 版只是提供远程访问，后端仍跑在本机，不设独立存储方案。

## 2. 数据模型与存储

### 2.1 目录布局（定稿）

两级存储：全局用户目录 + 项目内目录。

```
~/.flashsight/                        # 全局（Win 下为 %USERPROFILE%/.flashsight）
  config.json                         # 全局设置
  providers.json                      # 供应商 endpoint/key（前期明文，权限 600；TODO: 迁系统钥匙串）
  projects.json                       # 项目注册表（路径→pid，供主页快速进入）
  usage/calls.jsonl                   # 全局用量（append-only，每行一次调用，pid/sid 字段区分）

<project>/.flashsight/                # 项目级，跟项目走
  project.json                        # 项目 MCP/skill 开关、权限覆盖、环境变量覆盖（含 ignoreGlobal）
  sessions/<sid>/
    full.json                         # 完整历史（唯一��相源，只追加）
    runtime.json                      # 缓存：最近构建的运行时 history + 压缩记录（可重建、可删）
    patches/<turnId>.diff             # 每轮文件修改 diff（历史查看与回滚用）
    terminals/<tid>.log[.gz]          # 终端日志（旧日志压缩，见 §8）
```

- **模型配置归属**：不设全局“当前模型”；每个会话固化自己的
  `model/thinking/filePerm/autoMode/enterSend`，切换会话即切换这组值。
  全局只存 provider 连接信息 + 新建会话默认值。**刷新配置** = 重载 provider/
  工具表/系统提示词并递增配置版本，新 turn 生效，清连续失败计数，不改历史，
  会话固化的模型选择除非不兼容否则不被覆盖；操作记 systemOps。

### 2.2 Session full.json schema

- `meta`：sid、pid、cwd、创建时间、标题（见 §3.6）。
- `sessionSettings`：见上（会话固化）。
- `turns[]`：`turnId、time、role、modelSnapshot（当时模型+思考强度+配置版本）、
  permissionSnapshot、promptRaw、attachments（图片/文件引用）、toolCalls[]
  （name/args/resultSummary/resultRef/permissionDecision/terminalRef/subagentRef/
  truncated?）、planDelta、usage（in/out/cache/cost|quota|unknown）、streamSummary、
  ask{question,options,recommended,selection}|null、error|wake|refresh|compress 记录`。
- `systemOps[]`：切换模型、改设置、刷新配置、短视图/压缩事件（不在运行时 history
  里，但 full.json 里可复刻）。
- `snapshots[]`：每轮前 patch 栈指针（见 §5）。

### 2.3 运行时 history 构建（HistoryBuilder）

- 输入 full.json，输出 provider 消息数组：**短视图（最近 N 轮全量）+ 外层压缩摘要**；
  N 与 token 预算双封顶（起步 N=20 + token 上限，具体阈值进设置）。
- 超预算时对最旧若干轮调摘要模型生成 summary，原文不再进 history，
  full.json 保留 + systemOps 记 `compress{from,to,byModel,summaryRef}`。
- 提示词前缀装配顺序（定稿）：**前缀说明 → 环境 → 工具定义 → skills 与 MCP →
  对话历史 → 配置（模式、工具启用状况等）→ 新一轮对话**。
  其中“工具启用状况”只写两类：被禁用的系统工具、在运行期间启用状态变化了的
  自定义工具；其余不占 token。

### 2.4 Project 与“新建页”

- 项目 = cwd + 注册表项 + `<project>/.flashsight/project.json`。
- 主页“项目”列历史项目（名、路径、最后会话、最后活跃）；“新建”支持新建文件夹/
  打开文件夹/远程连接（远程连接 v1 只留入口，libssh2 预留，见 §10）。
- 新建标签页三选一：打开已有 / 在项目中新建会话 / 新建项目，复用主页“项目”逻辑。

## 3. Agent 引擎主流程（状态机）

```
idle ──send/append──▶ planning ⇄ toolCalling ──permission/ask──▶ waiting
  ▲                        │                       │
  │                        ▼                       ▼
  └──── autoNext ◀── turnDone ── suspendRequested（排队调用打完即停）
wake 来源：用户输入 / 后台终端退出 / 模型定时唤醒（用户可取消）/ 子代理完成
```

- **手动模式**：一轮结束即停，等用户输入。**自动模式**：turnDone 后 plan 未完成
  （嵌套规划子目标未闭合）自动开下一轮；后台终端退出、定时器到点自动唤醒。
- **挂起**：置位 suspendRequested，当前已发出 toolcall 跑完，不再发新调用；
  挂起中用户可改文件/改指令，恢复后 plan 不丢。
- **唤醒显示**：单次唤醒时长**不设上限**，用户手动取消；项目级可见待唤醒状态，
  会话输入框区域显示待唤醒信息；“中断生成”与“取消待唤醒”共用按钮位置、
  不同图标（不共用 face）。
- **ask 工具**：参数 `question + options[] + recommended（模型给的推荐/默认选项） +
  allowCustom`。手动模式弹 UI 等用户选；**自动模式不提供 ask 工具**（工具列表里直接拿掉）。
  防武断两层：系统提示词写死（不确定不下定论）+ 后端检测高风险动作
  （写文件/删文件/发网络/跑终端）且上下文不确定时自动降级为 `deferred` 等用户，
  在 UI 呈等待态。
- **防过拟合（默认关）**：检测器（归一化相似度/SimHash，阈值联调时定）触发后
  **不在后端截断**，只在对应输出块下方给出“截断/折叠”提示条，由用户点。
  不自动杀 turn、不自动截 toolcall。开关在会话设置。
- **子代理**：`spawn{task, model|inherit, cwd, permission≤父, timeout, depth+1}`；
  **只设单会话并行上限**（默认 4，设置-子代理可调），不设全局上限；
  递归深度上限 3；结果 summary 回父 + 全文进侧栏“子代理会话历史”；
  取消与超时落库。

### 3.6 标题与折叠 summary

- 标题：先用首句截断；当轮数 ≥3 或首轮模型自主 toolcall ≥10 次时，
  调摘要模型升级标题，摘要模型默认继承会话模型（可在模型偏好里另设）。
- 折叠 summary：每轮最后一次输出，提示词显式要求模型自带一段总结，
  折叠时显示该总结（失败回退首句截断）。

## 4. 工具层（内置 MCP；openWith 不在此）

| 工具 | 语义 |
|---|---|
| file.read | 部分读 offset/limit；二进制守卫（超阈值/非文本拒绝并提示） |
| file.write / diffWrite | 全写与 diff 写；diff 上下文对不上直接失败不猜；全量 diff 进 patches/ |
| file.ls/rm/find | ls、find/grep（大小写/忽略目录）；**rm = 直接删除**，确认由批准权限门承担，不进回收站、不二次弹窗 |
| terminal.exec/spawn | 前台（timeout）/后台；输出流式 poll；退出码落库；前台超 5min 自动转后台并通知 |
| network.fetch/search | fetch（method/headers/timeout/sizeCap/证书开关）；search 先调研开源实现，有就直接用，没有则接可配 HTTP 搜索 API（URL+key，预置 1~2 家） |
| vision.screenshot/readImage | 截图（屏/窗口/区域，权限门）/读本地图（压缩：最大边+质量，可设） |
| ask | 见 §3（含 recommended）；自动模式不下发此工具 |
| subagent | 见 §3（单会话上限 4，深度 3） |

- 所有写操作过两道门：**filePerm 三档**（每次询问 / 项目目录默认放行 / 完整 FS）
  × **会话文件锁**（§6）。拒绝给模型机器可读原因，不静默失败。

## 5. 回滚 / 分叉 / 遗忘

- **回滚（回溯）**：删除该对话及之后 turns，并用 **agent 自维护的 patch 栈**
  逆向还原文件（`patches/<turnId>.diff` + 新文件删除记录）。**不做每轮自动
  git commit，不污染用户 git 历史**。untracked 处理、二进制/重命名对不上时
  如实报错；终端副作用（已发出的网络请求等）不可回滚，UI 明确告知。
- **git 侧栏**：独立的手动工具（历史/分支查看、checkout/reset），允许手动回溯，
  与会话 patch 回滚互不干扰。
- **非 git 项目自动 `git init`**：允许，但默认关闭，开关在设置-权限-文件
  （或环境二选一，动工时定最终位置）。
- **分叉**：以某 turn 为起点复制 full.json 前缀 + 新 sid，原会话不动。
- **遗忘**：仅从运行时 history 剔除（全文忘/单条忘），full.json 标 `forgotten`
  不物理删除；压缩摘要需重建。
- 历史下“查看文件修改”：小按钮 → 该对话下展开 diff div（限高滚动，超长截断），
  点击跳侧栏文件查看。已完成 chat 默认折叠只留 summary。

## 6. 文件锁与权限矩阵

- 非完整权限：同一项目同时只有一个会话可写，其他会话写尝试返回
  `lockedBy(session)` + UI 提示；锁随会话关闭/切换���放（超时兜底）。
- 完整权限：走 OS 语义，不另设全局写锁。
- 权限切换即时生效并记 systemOps；低→高需显式确认。
- 黑/白名单优先级：黑名单 > 白名单 > 三档默认；拒绝可解释。
- 终端总开关在 settings-权限-终端；shell 顺序：bash > zsh(linux 替代) >
  pwsh7+ > PowerShell > cmd。

## 7. Provider / 模型 / 用量

- 供应商：OpenAI Chat、OpenAI Response、Anthropic Messages + 自定义 endpoint/
  header；套餐连接单独一类。v1 只保证“能连、能计次数”，额度余额显示能做多少
  做多少；provider 层预留 `quota` 字段与三态显示（钱/额度/unknown）。
  TODO：调研各家套餐需要的功能（含 opencode 内置项），后面直接用使用字段定义多家。
- 模型默认值：用户自定义 > API 获取 > 预置列表 > 保守缺省；一键获取失败回退上一级。
- 模型能力标签：vision/thinking/toolUse/contextLen（预置表 + API 获取后自动填）。
  **不支持 toolUse 的模型拒绝进会话**（创建时硬拒绝）；若供应商虚标能力导致
  运行时报错，原样透出，不归工具层兜底。
- 用量统计：`usage/calls.jsonl`，append-only，每行一次调用，字段宽松
  （ts/pid/sid/provider/model/in/out/cache/cost|quota|unknown/latency/toolCalls/
  error 等，有什么记什么）。读取容忍坏行/缺字段。聚合维度 provider × model ×
  time；图表照 design.json（12h/1d/7d/30d 柱状、扇形、token 表、成本表）。
  默认保留 90 天（设置可改），导出先做 CSV。

## 8. 终端子系统

- 两类：**用户终端**（可交互）与 **模型终端**（exec 前台 / spawn 后台），
  编号统一自增。
- 前台超 5 分钟自动转后台：终端页出现对应编号页，原 history 保留引用；
  后台退出触发 wake（自动模式）或徽标提示（手动模式）。
- shell 探测：启动时全量探测，按 §6 顺序选最优，**静默降级**（cmd 兜底必可用，
  不弹窗，只记日志）；用户可在设置显式指定 shell；终端页显示当前 shell。
- 日志：`terminals/<tid>.log` 保留退出码+时长；**超 10 轮前的旧日志自动压缩**
  （如 `.log.gz`），默认不删除，用户可手动删；**文件缺失不得报错**
  （graceful 降级）。磁盘压力封顶以后再议。
- 环境变量与 CLI 工具 key-value：**全局 + 项目覆盖**，项目可设“忽略全局”；
  终端 spawn 时合并注入；设置页显示每条来源（全局/项目）。

## 9. UI 页与事件

- 页面 IA 照 goal（主页/会话/新建页/设置双栏树/用量），桌面风。
- 会话页四区：顶部栏（tab/模型/思考强度/权限/自动挡/挂起）+ 左悬浮规划栏
  （plan 树 + git 摘要：branch/提交态/更新时间 → 跳侧栏）+ 中间历史
  （虚拟化一次 10 条；历史渲染一次、当前实时渲染；复制/分叉/回滚/遗忘/看 diff）
  + 右标签侧栏（用量&上下文、本地文件、文件操作、git 历史分支、子代理历史、
  toolcall 详情、历史输入详情）+ 底部输入框（/ 指令与文件补全上浮、markdown
  高亮不渲染、**默认 Enter 发送**/Shift+Enter 换行，可切 Ctrl+Enter）+ 默认隐藏
  终端区（集合页 + 编号页）。
- 无边框自定义标题栏（Win/Linux X11），默认横向 tab 可切垂直 tab，悬浮 tooltip
  全名。“探索”与“插件”入口保留但**灰色不可点**。
- i18n：默认跟随系统（中英先行），语言文件编译时预处理进 cpp，
  预留外部翻译渠道；主题多套配色、每套深浅色（WhaleUI `@media` 查询）。
- 快捷键：v1 先做键位基础设施（可配表），默认键位以后再排（有人用不上，不阻塞）。
- 后端→UI 事件：`stream.delta、turn.done、tool.update、permission.prompt、
  ask.prompt、plan.update、git.update、terminal.output/exit、lock.change、
  wake.fired、usage.update、config.refreshed、truncate.hint、subagent.done`。
  （注意 `truncate.hint` 只是提示，不是截断命令。）

## 10. 工程：构建 / 第三方 / 测试 / 文档

- xmake 目标：`flashsight`（主程序）、`fs_core`（后端静态库）、`fs_tests`（单测）。
- 第三方（定稿 + TODO）：
  - path/遍历：ghsl `filesystem`（C++14，以后无缝换 `std::filesystem`）。
  - HTTP：cpp-httplib（header-only，client+server 同库，利好以后 HTTP 分离）；
    TLS：mbedTLS；SSH/远程：libssh2（v1 预留，给远程连接）。
    TODO：开工前验证 httplib+mbedTLS 在 Win/Linux 的编译与 HTTPS 可用性，
    不可用则回退 libcurl。
  - markdown：md4c，fork 魔改输出 diff AST（tree-sitter 太重，不用）。
  - JSON：nlohmann/json（单头）。
  - diff：dtl（单头，配合 diff 写）。
  - git：调 git CLI（零依赖起步）。
  - 相似度：自研小函数（SimHash/归一化编辑距离，阈值联调再定）。
- 测试先行：FsApi 接口表定稿 → 单测覆盖状态机（手动/自动/挂起/唤醒）、
  HistoryBuilder（窗口+压缩+遗忘+前缀顺序）、权限矩阵（三档×锁×黑白名单）、
  回滚/分叉（patch 栈逆向 + 缺文件容忍）、终端晋升（5min 规则可注入时钟测）、
  用量聚合（JSONL 坏行容忍）。改完先自查再跑测试。
- 文档（`doc-internal/`）：`arch.md、session-history.md、agent-loop.md、
  permissions.md、terminal.md、provider.md、usage.md、ui-events.md、build.md
  （环境与选型验证记录）`。
- 笔记纪律：重要事项记 notes（编号递增、index/current 同步、tag 时归档）；
  工作区干净，临时数据只放 temp/。密钥明文存放为临时方案，已记 TODO，
  以后迁钥匙串。
