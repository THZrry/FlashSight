# UI 事件与页面（ui-events）

> 决策来源：goal §界面、addons §9；QA Q8、Q19–Q22。
> 前提：UI 按**不支持 JS 的 webview**规划，DOM 操作是全的；
> WhaleUI API 未完善，本文件只定“画什么、推什么”，不定控件 API。

## 1. 页面 IA

- 主页：探索（v1 灰色禁用）/ 项目（历史项目 + 新建：新建文件夹/打开文件夹/远程连接入口）/
  用量统计 / 设置 / 新建。
- 会话页（上下 + 双侧栏）：
  - 顶部栏：tab / 模型 / 思考强度 / 权限 / 自动挡 / 挂起（待唤醒与中断共用按钮位、不同图标）。
  - 左悬浮规划栏：plan 树 + git 摘要（branch/提交态/更新时间 → 跳侧栏）。
  - 中间历史：虚拟化一次 10 条；历史渲染一次、当前实时渲染；每条可复制/分叉/回滚/
    遗忘/看 diff；已完成 chat 默认折叠只留 summary（可展开）；输入框区域显示待唤醒信息。
  - 右标签侧栏：用量&上下文、本地文件、文件操作、git 历史分支、子代理历史、
    toolcall 详情、历史输入详情；可折叠。
  - 底部输入框：/ 指令与文件补全上浮；markdown 高亮不渲染；默认 Enter 发送、
    Shift+Enter 换行（可切 Ctrl+Enter）；截图/图片附件入口。
  - 终端区：默认隐藏；集合页 + 编号页；手动停止；显示当前 shell。
- 新建标签页：打开已有 / 在项目中新建会话 / 新建项目（复用主页项目逻辑）。
- 设置页：经典双栏（通用/模型/权限/其他四组，见 provider.md §5 与 goal）。
  探索/插件入口保留但灰色不可点；v1 无 `plugin.*`。
- 无边框自定义标题栏（Win/Linux X11，最大/最小/关闭 + 侧栏管理钮）；
  默认横向 tab，可切垂直 tab；悬浮 tooltip 显示全名。

## 2. 后端→UI 事件表（v1）

| 事件 | 载荷要点 | UI 行为 |
|---|---|---|
| `stream.delta` | sid, turnId, text | 当前对话实时渲染（diff 更新） |
| `turn.done` | sid, turnId, summary | 历史追加一条，折叠态 |
| `tool.update` | name, args?, 进展 | toolcall 详情 + 文件操作视图更新 |
| `permission.prompt` | 动作, 目标 | 批准弹窗（approve 档） |
| `ask.prompt` | question, options, recommended | 手动模式弹窗等用户选 |
| `plan_update` | planTree delta | 规划栏更新 |
| `git.update` | branch, dirty, time | 规划栏 git 摘要更新 |
| `terminal.output/exit` | tid, chunk / exitCode | 终端页更新；后台退出→徽标或唤醒 |
| `lock.change` | pid, holder | 写锁提示更新 |
| `wake.fired` | sid, source | 待唤醒徽标/输入框区信息 |
| `usage.update` | 调用用量 | 用量小视图 + 用量页更新 |
| `config.refreshed` | configVersion | 提示条“配置已刷新” |
| `truncate.hint` | turnId, 位置 | 输出块下加截断/折叠提示条（不自动截） |
| `subagent.done` | summaryRef | 子代理历史更新；自动模式唤醒父 |

## 3. i18n / 主题 / 输入

- 语言默认跟随系统（中英先行），语言文件编译时预处理进 cpp，预留外部翻译渠道。
- 主题多套配色、每套深浅色（WhaleUI `@media` 查询）；字体设置、代码高亮在设置-通用-外观。
- 快捷键 v1 只做基础设施（可配表），默认键位以后再排。
- openWith（文件管理器/vscode/vs/sublime/kate/jetbrains/系统默认）是右键下拉组合按钮，
  纯 GUI 功能，路径模板在设置-环境管理。

## 3.1 右键菜单

浏览器自带的右键菜单是**文档级默认行为**：只在某个节点上绑 `contextmenu` 是拦不住的，
没绑定的区域（空白、卡片、侧栏）照样弹出「后退 / 重新加载 / 另存为 / 检查」。
所以两个 DOM 适配器都在启动时装一个 `document` 级 `contextmenu` 监听，**一律
`preventDefault()`** —— 包括文本输入框：菜单要在所有地方长得一样，剪切/复制/粘贴
由应用自己的菜单项提供（`IDom::exec_edit`，粘贴走异步剪贴板 API，菜单点击本身就是
合法手势）。

应用菜单的状态在 `AppState::ctx_*`（`ctx_open / ctx_x / ctx_y / ctx_kind / ctx_id / ctx_label`），
渲染在 overlay 层，`position: fixed` + 内联 `left/top`。**位置来自指针**——适配器把
`contextmenu` 事件的值改成 `"x\ty"`（视口坐标），而不是锚定某个控件：右键菜单属于
"这一次点击"，不属于某个按钮。

按被点对象给不同的项，只保留真正能用的：

| 对象 | 菜单项 |
|---|---|
| 消息 | 复制 / 从这里分叉 / 回滚到这里 / 忘记这一轮 |
| 标签页（顶部模式） | 重命名 / 关闭 |
| 会话行（左栏导航 + 项目卡片） | 打开 / 重命名 / 删除会话 |
| 输入区 | 剪切 / 复制 / 粘贴 / 全选 / 清空输入 |

关闭方式：Esc，或按到菜单外的任何地方（`IDom::bind_outside_click`）。**不能用全屏
遮罩层**——那会把下一次右键吃掉，表现为"右键别处什么都不出现"；`bind_outside_click`
只关菜单、不吞事件，所以右键另一行会直接换成那一行的菜单。

## 3.2 流式渲染

后端走真正的 SSE：`NetClient::request_stream` 把字节按到达顺序交给回调，
`ProviderClient` 用 `SseReader` 拼行（`data:` 行可能被 read 边界切断），每段文本立刻
回调 → `AgentLoop` 发 `stream.delta` 事件。

**前端不能用轮询**。浏览器的定时器在"被遮挡/后台"的页面里会被压到 ~1Hz（实测
`setInterval(120ms)` 的真实间隔是 1031ms），于是 UI 每秒只能取一批事件，模型每秒
产生上百条 delta，前端永远追不上——表现就是"生成完了才一点点喂过来"。所以：

- `GET /api/stream` 是 SSE 长连接（`text/event-stream` + chunked），事件产生时由
  `condition_variable` 唤醒写入，不经过任何定时器。`/api/events?since=N` 仍保留给
  测试和简单客户端。
- 前端 `EventSource` 在 `poll()` 里**只负责确保连接已打开**，不再拉取事件。
- **重绘也由数据驱动**：delta 到达时直接 render（SSE 帧是普通任务，不受定时器节流），
  只加 25ms 下限防止一次连发过多；不靠 `setInterval` 刷。
- 非流式仍然支持：服务端忽略 `stream:true` 只回整段 JSON 时，`saw_sse == false`，
  走正常解析再补发一次 delta（否则回复会凭空消失——测试就是这么抓到的）。
- 计时：`turn.done` 带 `startedAt/endedAt`（后端时钟，比 UI 看到第一个 delta 更准）；
  流式期间显示已用时长，结束后常显"时长 · 结束时刻"，只有操作按钮 hover 才出现。

## 3.3 快捷键 / 弹窗失焦 / 滚动状态

**全局键带修饰符**。`IDom::bind_global_key` 的回调值是 `"Ctrl+Shift+W"` 这种形态，不是
裸键名——快捷键表里写的就是 `Ctrl+W` / `Ctrl+T`，适配器只报 `"w"` 的话这些绑定无法实现
（设置页存下来也没人读，用户称之为"摆设"）。三个适配器都做了：

| 适配器 | 做法 |
|---|---|
| wasm / webview | 文档级 `keydown` 里拼 `Ctrl+/Meta+/Alt+/Shift+` 前缀 |
| WhaleUI | 新增文档级 `keydown` 监听（引擎的结构化事件带 `WHALEUI_MOD_*`）；window 级回调**仅在它不存在时兜底**，否则每个快捷键会触发两次 |

应用侧在 `render()` 的全局键处理里按 `shortcuts.<动作>` 匹配：关闭标签页 / 新建标签页；
`shortcuts.发送消息` 里含 Ctrl/Meta 时，输入框切到 Ctrl+Enter 发送（否则尊重会话自己的
Enter 设置，不越权覆盖）。

**弹窗失焦即关闭**。`comp::modal` 的 backdrop 点击只是其中一条路径；层叠上下文里的
模态还要绑 `IDom::bind_outside_click`（backdrop 只覆盖渲染它的那个元素的可视区，从标签
页上方打开时点标签条不会命中）。两条路径最终都走同一个状态清理，所以 keyed 节点的
`exit_anim` 一定播得出来。

- **权限门（permission.prompt）的"关闭" = 拒绝**：loop 停在 `kWaiting` 等一个答复，
  静默关掉会挂住整轮。Esc、backdrop、失焦三条路径都显式 `session.approve(yes:false)`。
- confirm 弹窗（删除/回滚/遗忘）失焦只清状态，**不执行**动作。

适配器里的 outside 判定**只豁免锚点**（`.model-field` / `.model-btn-wrap` /
`[data-keep-popup]`）。原来豁免的是整个父容器（`host.contains(t)`），于是 popup 的兄弟
节点全被当成"内部"，点哪儿都不关——表现就是"关不掉"。

**会话滚动状态**。`hist_scroll` / `hist_follow` 属于 `SessionView`，切标签时随视图一起
存取。三个坑：

1. 读取时机——切走前要从 DOM 读一次当前 offset，不能只在滚动回调里更新。
2. **webview 适配器的写要同步回写镜像**：它读 `scroll_top()` 读的是 C++ 侧镜像，而镜像
   由 JS 异步 push 更新，`set_scroll_top()` 之后立刻切标签会读到旧值，保存的就是旧位置
   （wasm 适配器直接读 `el.scrollTop`，没这个问题）。
   - **镜像的键是节点 id，不是回调 id**。glue 里 `push('scroll', el.__fsEdgeCb, …)` 用的是
     回调句柄，而宿主 `on_push` 按 `slots_[id]` 写节点槽位——位置永远停在 0，于是每次切页
     都保存 0、恢复 0。必须写 `push('scroll', i, …)`（`i` 是那次 `__fsDomCall` 的节点索引）。
   - **滚边/滚尾绑定传的是"启用"标志，不是 id**。`bind_scroll_edges` 原来传 `""`，JS 里
     `parseInt("")||0` 得 0，监听器开头 `if (!el.__fsEdgeCb) return` 直接退出：滚边回调、
     镜像、`at_top` 加载更早的轮次、终端的 at-bottom 在 webview 里全是死的。现在传 `"1"`。
3. **恢复必须在"同一次 render"里完成，不能延后一拍**。`request_history_scroll_restore()`
   只是记下目标，真正的 `set_scroll_top` 在 `render()` 的 `patch()` 之后执行：
   先读一次 `scroll_height()/client_height()` 强制布局（否则浏览器还按旧高度钳位），
   再赋值。这样浏览器第一次绘制时位置就是对的。
   - 以前写 `dom_.schedule(0, …)` 延后一拍：内容先以 scrollTop=0 画出来，再跳过去，
     用户看到的是"先到顶、再蹦到位置"（"切换页面还是不对，这次是到顶上了"）。
   - 恢复期间 `restoring_scroll_` 抬起，滚动回调不把钳位后的值写回 `hist_scroll`；
     两拍后自动落下。
   - webview 里 patch 与 `scrollTop` 是两次 `ExecuteScript`，所以 glue 的 `scrollTop` 分支
     会立刻赋一次、再在 `requestAnimationFrame` 里补一次（第一次绘制前）。
   - 实测（WebView2 + CDP，见 notes <87>）：长会话滚到 500 → 主页 → 切回仍是 500；
     滚到顶会加载更早的一页（30 → 47 条）。

## 3.4 子代理事件的去向

子代理事件带 `sid=父会话` + `subagentId` + `subagentKind` + `fromSubagent`。
`apply_session_event` 据此分流：

- `complete-step` —— 本轮任务在推进，**进主会话**（用户该看见自己的任务在走）。
- 其他（`local-research` / `research` / `explore` / `check-files`）—— 旁路工作，
  **只进侧栏"子代理"面板**（其转录、耗时、引导都在那儿）。把它们的 stream/tool 行铺进
  主转录会把真正的回答淹掉。

## 3.5 MCP 与 Skill 是两件事

用户报告"gsd 在 MCP 里有启用，指令却跑到 Skill 里"。根因是**探测错了目录**：
`mcp_detect` 扫的是 `~/.claude/skills`、`~/.claude/commands` 这些 **Skill 目录**，
把每个子目录注册成一条 MCP 条目（用户配置里 72 条 `mcp.entries` 全是 `gsd-*` 这类，
且和 `skills.entries` 重复）。

| | 是什么 | 谁调用 | 探测方式 |
|---|---|---|---|
| MCP | 协议端点（一条命令或一个 URL） | **模型**自主调用其工具 | 读配置文件里的 `mcpServers` / `mcp.servers` / `servers` |
| Skill | 一段说明（SKILL.md） | **用户**用 `/名称` 调用 | 扫目录里的 `SKILL.md` / `<name>.md` |

- 路由分开：`mcp.detect` 与 `skill.detect`（Skill 页原来调用 `mcp.detect`，所以永远
  只能找到 MCP 条目，skill 只能手动导入）。
- 条目带 `kind`（`server` / `dir`）与 `server`（command/url/args/env），MCP 卡片显示
  真实命令行，不再只显示一个目录路径。
- **启动时迁移**：旧表里 `dir` 落在 skills/commands 目录下的条目会被导入 skill 注册表
  并从 MCP 表移除，然后写回设置。这是一次性的，跑完表里就干净了。

## 3.6 规划面板 / 子代理面板 / 顶栏标签

**规划与 todo 是两件事，面板也分开**：

| | 是什么 | 谁维护 | 用途 |
|---|---|---|---|
| todo（任务清单） | 主代理自己的待办（`todo_add`/`todo_check`） | 模型，一条一条勾 | 进度自述 |
| plan（规划树） | `{goal, steps:[{title,status,note}]}` | 模型（`plan_update`） | **`should_auto_continue` 读它决定是否续跑** |

规划面板过去只在点「规划」按钮时拉一次（`plan.get`），**宿主发的 `plan.update` 事件没人听**，
所以模型中途写好的规划看不见——「面板没有显示规划」。现在 `apply_session_event` 处理
`plan.update`（后台会话落到该会话的 stash 视图），面板实时显示：goal、每步的状态标记
（✓/▶/–/!/--）、以及 `已完成 n / 总数`——**和自动续跑用的是同一个条件**。

手动增删改步骤**已删除**：规划是模型的工作状态，脚本按状态决定是否继续，手改等于写一份
模型没同意过的树（用户原话"手动添加也是画蛇添足"）。保留的是**评审**（通过/驳回/按意见修改），
那才是用户能负责的表达方式。

**子代理面板**：运行中显示**正在做什么**（`live_text`，由带 `subagentId` 的 `stream.delta`
与 `tool.start` 累积，只留尾部 2KB），对话记录里连 tool call 的名字一起列出（以前只列文本，
工具行整段消失，看起来"没干活"）。「引导」从"点一下发一句固定的『继续』"改成**输入框 + 发送**：
运行中的走 `subagent.steer`，**已结束的走 `subagent.followUp`**（同一 id、带原转录再跑一轮，
父代理/用户因此可以在一份上下文里继续加活）。

**工作过程（work-block）**的展开区不再是 10 行的小窗口（`min(10*1.55em+3em, 50vh)`），
改为 `min(28em, 70vh)` 并参与页面滚动；工具详情 `tb-body` 从 220px 提到 60vh。

**顶栏标签**：`tab_strip` 拆成两个槽——`tabs-fixed`（主页 + 不可关闭的标签）与
`tabs-scroll`（会话标签 + "+"）。一个整体滚动的条在开十几个会话后会把主页推出左边缘，
就没有回主页的路了。

## 3.7 子代理的可见性：卡片 / 侧栏记录

一条子代理是**完整的一轮 agent loop**，转录可以几千行。因此：

- **侧栏卡片**是摘要：状态、耗时、`live_text`（正在做什么）、摘要、引导输入框。
- 卡片**可点击**，在**侧栏**打开该子代理的完整记录（一个 `kind="subagent"` 的侧栏标签，
  带 `subagent_id`，可多开、可关闭）。
  **不是全屏页面**——用户明确要求放侧栏：记录属于"看会话时顺手看"的东西，
  而且全窗口页面还没法关回会话。
- **工作过程里的 `subagent_spawn` / `subagent_follow_up` 行带 `subagent_id`**
  （从工具结果的 `id` 解析），行上给一个「打开记录」按钮，打开同一个侧栏标签。
  没有这条链，工作过程只能证明"跑过子代理"却看不到它做了什么——**子代理一结束就
  永久无法在 UI 里看到**，这是设计缺陷而不是样式问题。
- `subagent.done` 到达时，已打开的记录标签会重新拉取，把最终摘要补上。
- 保留上限：`prune_finished(sid, 40)`，所以**很久以前的**记录会被清掉（内存所限）。

**列表与详情的分工**（这也是一处真实的爆炸）：`subagent.list` 曾经把每个子代理的
`full`（含每条消息里的 system prompt 副本，约 10 KB/条）内联返回——**3 个子代理就
超过工具输出上限**，最后一个条目被整段截掉。现在：

| 调用 | 内容 |
|---|---|
| `subagent_list` | 每项只有 id/kind/status/task/summary/时间 + `transcriptMessages`（条数） |
| `subagent_detail(id)` / 路由 `subagent.detail` | **一个**子代理的完整记录（含转录） |

## 3.8 `assistant.interim`：工具轮之间的过程说明

一轮对话里，模型在每次工具调用**之前/之间**都会说话（"先看一眼配置"）。这段文本：

- **不是答案**——答案是最后一轮说的话；
- 以前**只保留最后一轮**，中间的说明一关会话就没了，而且流式期间被粘进答案气泡里，
  看起来像模型在重复自己。

现在每轮结束发 `assistant.interim{text, round}`：UI 把它归到该条消息的工作过程里
（`ChatMsg::interim`，折叠区里"过程说明"一节），**并随 turn 落库**（`turn.interim`），
所以重开会话还能看到。折叠标题也写 `N 段过程说明`，不展开就知道有内容。

**一个曾经让它整体消失的条件**：工作过程的渲染门槛是
`thought || tools || interim`——**只有旁白、没有工具块的那种轮次**必须也能渲染出折叠块，
否则"过程说明"没有容器。同理，**纯工具调用的轮次没有 `stream.delta`**，所以
`tool.start` / `tool.update` 会**自己创建**该轮的 assistant 消息（`ensure_turn_message`）：
不创建的话工具块被丢弃（工作过程不显示），而计时器也渲染不出来（它挂在这条消息上）——
这正是"有时候不显示计时，切页面才有"的原因。

**高度**：工作过程与其展开的工具详情都**不再设上限**，随内容增长、由页面滚动。
试过的 10 行 / 28em / 78vh / 220px / 65vh 都让"看长结果"变成在小盒子里滚动。

## 3.9 输入区右侧：模型 / 思考 / 挂起 / 发送

- **模型按钮带供应商**。`主名 · 供应商名`（供应商 dim、非等宽），或从
  `model.allEnabled` 的 `providerName` 取。只写模型名在"两家供应商有同名模型"时是有歧义的，
  而真正决定请求去哪儿的正是供应商。
- **模型选择器打开在"当前模型所在的那一页"**。左列是供应商列表，打开时默认
  `provider_selected`/第一个供应商会让当前模型落在别的页上（"不会自动放到选择的那一页"）。
  `open_model_picker` 记下 `model_picker_focus`（会话模型，或设置项当前值），
  `allEnabled` 返回后把左列切到该模型的供应商。
- **思考档位是 ONE 控件**。以前是一个空的图标按钮（没有 handler，按了没反应）+ 一个
  原生 `<select>`（同一个值），看起来就是"两个思考档位，其中一个还是死的"。
  现在只有一个图标按钮：**每一档一个专属图标**（关=power-off、最低=feather、低=gauge、
  中=brain、高=fire、超高=gauge-high、最大=infinity、自动=wand-magic-sparkles；
  **未知档位统一用灯泡**，绝不空白），点开是一个列表，每行同样带自己的图标与勾选。
  窄容器时 `.it-label` 收起 → 只剩图标。
- **思考档位来自模型表**。原来合成器用 `thinking_levels_for(模型名)`——那是**硬编码白名单**
  （o1/claude/gemini/deepseek-r…），任何自定义/新模型都返回空，于是控件**根本不渲染**
  （"没做思考强度按钮"）。现在用 `model.allEnabled` 里该模型的 `thinking.levels`
  （用户在模型设置里配的、或 API 返回的），猜名只作为模型表的兜底。
- **挂起只在"能挂起"时出现**。空闲会话上常驻一个 挂起 按钮，按下去只是置了个没人会读的
  标志（"功能疑似没做全"）。现在 `turn_running || suspended` 才渲染；已挂起时文案是
  **恢复**，点它真的从停下那一步继续（见 agent-loop.md §4）。
- **挂起不乐观翻转**。后端拥有状态：回合可能正在跑工具，要等当前调用返回才 park。
  点击只发请求，`refresh_loop_state` 回读 `session.loopState` 决定按钮文案；请求在飞时
  按钮禁用（否则连点会发出 suspend+resume 两条相反请求）。
- **发送 / 中止是同一个按钮**。`turn_running` 来自 `turn.started` / `turn.done` /
  `loop.interrupted` / `loop.paused` 与 `session.loopState`，为真时显示**中止**
  （`session.suspend`：当前工具返回后 park、保留转录、可继续——没有更快的硬中断）。
  注意 `suspended` **不算** running：停住的回合不该继续显示"中止"。
- **没有手动"定时唤醒"按钮**：排程是模型的动作（`session.schedule`）。用户唯一相关的操作是
  **取消**一个已排的唤醒，它只在真的有待唤醒时出现在挂起旁边。

## 3.10 输入框的内联补全（markdown 提示条已删除）

两条触发规则（都是**提示**，未选中前不改一个字）：

- 输入**第一个字符就是 `/`** → 命令/skill 列表（`command.list`）。此后 `/br` 继续过滤。
- `/` 或 `./` 出现在**别处** → 文件/文件夹候选（`complete.path`）。`./x` 相对项目根；
  裸 `/x` 是文件系统根；宽松/完全放行模式下才允许项目外候选，且 60 条封顶、跳过隐藏项。
  路径片段的起始按"词首"算：`看下 ./src` 补的是 `./src` 而不是整行。

键盘：↑/↓ 移动选中（环形），Tab 接受，Esc 关闭。接受后候选被插入并立刻关闭菜单。

**两个必须同时存在的细节，少一个就表现为"按键没反应"**：

1. **接受结果要直接写进 DOM。** `accept_complete()` 改的是 `st_.session_input`，而框架
   *故意*不给获得焦点的 textarea 回写 `value`（否则每次敲键都会把光标弹回末尾）。
   因此接受后必须自己 `dom_.set_attr(ta->dom, "value", text)`（与 `send_message()`
   清空输入框同一手法），否则用户看到菜单关了、文本没变。
2. **浏览器不能先处理这些键。** 两个浏览器后端在元素带 `data-keys-capture` 时对
   Tab/↑/↓/Esc 调 `preventDefault()`；否则 Tab 会先移动焦点、方向键先滚页，按键根本
   到不了 app。该属性**只在补全菜单打开时**由合成器加上——这不是给所有输入框的
   全局拦截，别处的 Tab 导航与方向键滚动必须保持原样。

**markdown 提示条（已删除，勿重加）**：textarea 无法给局部文本上样式，而本 UI
"打字不重绘"（重绘会跳光标），所以边打字边渲染是做不到的；退一步做的"上方提示条"
被判为无用（"还不是实时的……甚至显示还会让输入框失去焦点"）。整套
（`md_hint_for_inner` / `maybe_update_md_hint` / 状态字段 / `.composer-mdhint` 渲染）
已移除。要再动这个需求，先解决"输入框内局部样式"本身，而不是再挂一条提示。

## 3.11 编辑并重发 / 停止后的消息定格

**编辑并重发 = 先 arm，发送时才截断。** 动作按钮挂在那条 user 消息上
（`chat_message(..., can_edit)`），并且只给**最后一条** user 消息：

- `chat_message` 的 user 分支以前在拼完 meta 后直接 `return`，动作行只在 assistant
  分支生成，所以 `编辑` **从来没渲染出来**（"4 并没有修好"就是它——按钮不存在）；
- 点击只把文本放回输入框并记下 `edit_turn_id`，**不动历史**，同时显示
  "正在编辑上一条消息 / 取消编辑" 条；
- 真正发送时 `send_message()` 先 `session.trimHistory`，再在回调里把本地转录从那一轮
  截掉，然后走原来的 `dispatch_message()` 发送。这样点了编辑又改主意、或者 trim 失败，
  都不会留下重复的问题；
- 只允许最后一条 user 消息：改更早的问题会让它之后的所有回答悬空（那是 回滚 的活）。

**park（停止/挂起）不发 `turn.done`**，所以最后一条 assistant 消息不会得到"结束"事件。
`loop.state = suspended` 必须自己定格：`streaming=false`、填 `ended_at`；一个字都没产出
的空消息直接丢弃（否则转录里留一个转圈的空气泡）。同理，`loop.paused` 只处理一次
（曾经的第二个 `else if` 分支永远走不到）。

**`loop.interrupted` 是同一类事件的另一半**（provider 失败 / 工具抛异常）：它同样没有
`turn.done`，所以也要定格（否则"history 一直显示加载中"而按钮已经变回发送）。区别是
原因必须留在消息上（`ChatMsg::error` → `.turn-error` 行）：toast 三秒就没了，转录却要
能解释"为什么停在这里"。首轮就失败时**还没有 assistant 消息**，此时要补一条只有错误
的消息，否则问题后面什么都没有，看起来像发送被吞了。

**`session.loopState` 的回复必须只认最新一次请求**。打开一个会话会发起一次查询，而回合
可能在它返回前结束；晚到的 "running" 会把已经恢复成"发送"的按钮又改回"中止"。
`refresh_loop_state()` 用序号丢弃过期回复；`session.turns` 的回复同理**不能整表覆盖**：
发送是乐观追加的，磁盘上的转录比屏幕旧，整表覆盖会把刚发出的消息抹掉（"发送以后
history 没有反应，切页面才显示"）。合并规则：保留本地 `turn_id` 为空、且文本不在回复里
的消息，回复为底、这些消息追加在后。

**工具块的"输入"是独立字段**。`tool.start` 里的 args 曾经写进 `detail`，而 `tool.update`
会用结果覆盖 `detail`——于是完成的 read_file 只显示返回 JSON，永远看不到读了哪个文件。
现在 `ToolBlock::input` 单独存参数，展开后是"输入 / 返回"两段（只有一段时不加标签）。
`turn_to_message` 从 `tc[k].args` 恢复它，所以重新打开会话也还在。**空的 `{}` 不算输入**：
组件和两个填充点都会跳过它（否则每一行都多出一个只有两个括号的分段）。

**长文本横向滚动：工具体是"有界窗口"，不是"整页那么长的一条"。**
`.tb-body` 曾经 `overflow: visible` + `pre`（长行被外层 `overflow: hidden` 裁掉），后来
改成 `overflow-x: auto` 但 `max-height: none`——条是有了，可一条 200 行的 JSON 把横向
滚动条推到几千像素下面，鼠标根本够不着。现在的规则：

- `white-space: pre`（不折行：400 列折起来更糟）+ `overflow: auto`（两轴）+ 轨道透明、
  hover 才显拇指的细滚动条；
- `max-height: min(40vh, 26em)` + `overscroll-behavior: contain`：条永远贴在可见窗口的
  边缘，滚动到底也不会把整页带走。

**记录不能撑宽记录区**。`.history` 是 `overflow-x: hidden; overflow-y: auto`：一个宽表格
曾经把整个对话栏顶出横向滚动条，读者在看答案时整列会左右跑。表格由 markdown 包一层
`.md-table-wrap`（自己横向滚，`table { width: max-content; min-width: 100% }`）。既然历史
不再横滚，段落里的超长不可断词统一 `overflow-wrap: anywhere`，否则会被裁掉而不是折行。

**状态圆点的链条**（项目/会话/标签三处必须同时成立）：

1. 会话行的 running / loopState / terminalCount / autoRound 来自 `session.list`。它只在
   加载时取一次，所以"正在干活/开着终端"时圆点不动。现在这些事件会
   `mark_sessions_dirty()`，`pump()` 里 800ms 合并成一次刷新（terminal.output 故意不标记：
   刷屏的终端会让列表每秒请求）。
2. 当前会话那一行用 UI 的实时状态覆盖（`live_session_row`），点下去立刻变色。
3. 后台标签的 `SessionTab::dirty` 从来没人写：后台的 turn/流式事件现在点亮它，切过去清除。
4. `subagent.started` 之前完全没处理：新起的子代理没有行，它的流式文本按 id 找不到宿主，
   徽标也不涨——面板打开后才启动的子代理等于隐身。
5. **done / failed 是"新闻"，不是状态**。后端的 `loopState` 一旦是 `turnDone` 就永远不变，
   所以蓝点曾永久亮着（看过了、切页了都还在）。`dot_for()` 用 `session_seen_ms_` 过滤：
   打开会话、以及你正看着的那一轮结束/中断时记下"已看"，行上 `updatedAt` 不晚于它的
   done/failed 不再显示；之后再有事就是新的新闻。
   **时间基准必须一致**：`now_ms()` 是单调时钟（steady_clock / performance.now），
   行里的 `updatedAt` 是 epoch —— 用 `wall_ms()` 比较，否则"看过了也不消失"照旧。

**余额 / 额度**（`provider.balances`）：没有统一标准，所以按 **endpoint** 识别供应商
（改名成"我的 API"也还能认出 api.deepseek.com）。支持的：DeepSeek `/user/balance`、
OpenRouter `/api/v1/credits`、Moonshot `/v1/users/me/balance`、SiliconFlow `/v1/user/info`、
OpenAI 系 `/v1/dashboard/billing/credit_grants`。其余如实写"不支持"；请求失败写
"获取失败：<原因>"，**绝不补 0**。刷新：进入用量页（30s 节流）、每轮 `turn.done`
（8s 下限）、切换用量范围/模型。解析在 `provider/balance_probe.cpp`，用 FakeNet 单测。

**侧边栏默认面板**：`AppState::make()` / `default_session_view()` 把第一个面板设为激活
（原来 `active_side` 为空，kind 兜底成 usage——内容在、标签全不高亮，看起来像"什么都没开
却在显示统计"）。另外 `views_.get_or_create(sid)` 造的是**裸 SessionView**（没有 panels），
`load_session_view` 会补回默认面板，否则从后台会话切回来时面板会是空的。

**工作流是"模型规划 → 用户允许 → 侧栏看进度"**，不是侧栏里的启动器：

- 工具 `workflow_plan {goal, steps:[{title, task?, kind?}]}` 只**提交**计划：后端落成
  `state="proposed"` 的运行实例（每步一个 complete-step 子代理），然后停住。
- `workflow.approve` 是唯一真正开始执行的地方（`workflow.reject` 结束它）。任何别的路径让
  模型直接跑流水线，都违背了"先给人看计划"。
- 侧栏面板：上面是待允许卡片（目标 + 编号步骤 + 允许/拒绝），下面是运行的步骤进度（失败可
  重试/跳过，运行中可引导）。`workflow_proposed` / `workflow.step` 等事件会触发节流刷新，
  计划出现不用重开面板。

**终端记录必须落盘**。终端进程活不过本进程，但"开过哪些终端、打印过什么"要活过重启：

- spawn 时写 `<session>/terminals/<tid>.json`（元数据）并把 PTY 输出追加到 `<tid>.log`；
  退出时把 status/exitCode 冻结进 json。
- **输出必须从 `TerminalManager::feed_screen` 的 sink 写，不是从 `tick()` 写**：tick 可能再也
  不跑（命令一结束就关应用），那条尾巴就丢了——"重建时不完整"的真因。sink 是**实例成员**
  （不是 static：静态 sink 会活过它的 FsApi，单测里直接构造 TerminalManager 时调到已析构对象）。
- **历史"压缩"但不丢**：`<tid>.log` 满 4MB 就轮转成 `<tid>.<epoch>.log`，链条一直留着；
  用户输入单独写 `<tid>.input.log`（程序不一定回显，而"我敲过什么"正是搜索最常找的）。
  `terminal.search {tid,q}` 扫**输出链条 + 输入日志**，返回 `{file,line,text}`。
- `terminal_list` 先把磁盘记录装回注册表（`add_saved`：强制 exited、不覆盖活动终端、抬高
  tid 序号以免复用日志文件名）——**列表是 UI 唯一入口，不在注册表里的终端既不能显示也不能回放**。
- `terminal_grid` 对没有进程的终端从**整条日志链**重建（剥 ANSI、CRLF 归一、单独的 `\r` 取最后
  一段，保留最后 2000 行）；真的没有任何记录时返回**空网格**而不是 "not found"——刚恢复的终端
  允许是空的，那个错误只是看起来像 bug。

**顶部标签条的滚动条**：`.tabs-scroll` 以前把滚动条藏掉（`scrollbar-width:none`），开十几个
会话后根本看不出还有别的标签。现在 4px、hover 才显拇指（VS Code 的做法），高度只占 2px。

**余额查询是并行的**：`provider_balances` 每个供应商一个线程（每家 5s 超时），总耗时 ≈ 最慢
的一个而不是求和；UI 侧本来就是 callback 式异步（web 用 fetch、桌面用 worker 池），面板先画
"查询中…"再落数据。

**供应商图标按 endpoint 认品牌**：列表行原来没有图标元素（模板网格有，列表没有）。现在每行是
`provider_brand_for_endpoint(endpoint, name)`：先看 endpoint（改名成"我的 API"也认得 api.deepseek.com），
再看名字，最后用首字母——绝不出现空白方块。

## 4. 性能与空态（验收条目）

- 历史虚拟化（一次 10 条）、diff div 限高滚动超长截断、流式渲染 diff 更新（md4c diff AST）。
- 空项目/空会话/无网络/长列表/超大 diff 均要有空态与错误态，不白屏。
