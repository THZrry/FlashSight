# Agent 主循环（agent-loop）

> 决策来源：addons §3、QA Q6–Q10。实现位置：`src/agent/`。
> **上下文怎么拼、为什么这么拼，见 `agent-context.md`**（本文只管状态机与流程）。

## 1. 状态机

```
idle ──send/append──▶ planning ⇄ toolCalling ──permission/ask──▶ waiting
  ▲                        │                       │
  │                        ▼                       ▼
  └──── autoNext ◀── turnDone ── suspendRequested（排队调用打完即停）
```

- `planning`：拼前缀（见 agent-context.md §2）→ 调模型 → 得 plan/toolcalls。
- `toolCalling`：逐个过权限门（见 permissions.md）→ 执行 → 结果回填。
- `waiting`：等 permission 批准或 ask 回答；自动模式下 ask 不下发（见 §3）。
- `turnDone`：落库（full.json 追加、patch 存盘、用量记一行）→ 手动模式停，
  自动模式若 plan 未闭合（嵌套规划子目标未完成）自动 `autoNext`。

### 1.1 工具调用组（一次回复多个调用）

一条 assistant 消息可以带多个 toolCall，协议要求**每个调用都得到一条结果**。
旧实现只把结果塞进一条 user 消息（Anthropic 形状），`[OI]` 端点会忽略它，
模型于是反复重发同一调用直到上限。现在：

1. assistant 消息（含全部 toolCalls）先进 messages；
2. 逐个执行，每个结果单独一条 `role:"tool"` 消息（带 `toolCallId`）；
3. 全部完成后一次性回填给模型。

**审批门**按整组处理：第 N 个调用要批准时，整组和已产生的结果存进
`gate_messages_/gate_calls_/gate_index_`；`approve(true)` 执行它，
然后**继续执行剩余的调用**，最后统一回填。拒绝时把 `permission-denied`
作为该调用的结果回填（不静默）。

### 1.2 边界

| 闸 | 默认 | 行为 |
|---|---|---|
| 单轮工具往返 | 60 | 结束本轮并在回答里注明，不再调模型 |
| 连续相同调用 | 3 | 结束本轮并注明"repeated the same tool call" |
| 自动续跑轮数 | 12 | 发 `auto.halted`，停 |
| 工具参数 JSON 非法 | — | 不执行，回填 `invalid JSON` 让模型重发 |

前三者是"不能进展就必须停"的兜底：无人值守的会话烧的是用户的钱。

## 2. 手动 / 自动模式

| | 手动 | 自动 |
|---|---|---|
| turn 结束 | 停，等用户输入 | plan 未完成自动开下一轮 |
| 后台终端退出 | 徽标提示 | 自动唤醒继续 |
| 定时器到点 | 徽标提示 | 自动唤醒继续 |
| 子代理完成 | 徽标提示 | 自动唤醒继续 |
| ask | 弹 UI 等用户选 | 不下发 ask 工具 |

自动续跑时**必须**把模型上一条回答放进历史（否则看起来是用户凭空说"继续"），
并重新生成状态块（规划/清单的最新状态）。

## 3. ask 语义

- 参数：`question + options[] + recommended（模型推荐/默认选项） + allowCustom`。
- 手动模式：**loop 挂起本轮**（state = waiting），UI 用 `ask-panel` 占据输入框区域：
  标题（含 `1/x` 计数）+ 问题正文 + 选项行（单选圆点 + 推荐标记）+ 自定义输入 +
  **发送按钮**。点选项只是选中，必须点"发送回答"才提交（避免误点即作答）。
- 一个回复里可以有**多个 ask**：`run_tool_group` 把它们合并成一个 `ask.prompt`
  （`asks[]`），UI 用"下一个"顺序展示；`answer_ask` 按 askId 收集，**全部答完**
  才把每个结果依次回填并 `run_loop`。只答一部分时本轮保持挂起。
- 选择作为**该次 ask 调用的工具结果**回填。`askId` 递增，过期回答被拒绝。
- **工具本身不回答**：它只返回 `awaiting_user` 让 loop 停车。第一版工具发完事件
  就返回 `asked:true`，模型以为用户已答、本轮继续跑，选择从未回填——这就是
  "ask 静默失败"。
- 自动模式：工具列表里直接拿掉 ask；防武断两层：
  1. 系统提示词写死（不确定不下定论，选可逆路径，需要拍板处停下）；
  2. 后端检测高风险动作（写文件/删文件/发网络/跑终端）且上下文不确定时
     自动降级为 `deferred`，UI 呈等待态等用户。
- 高风险动作表初版：写文件、删文件、发网络、跑终端。

## 3.1 自动轮（auto mode 的续跑轮）

- 每轮开始前发 `auto.next{round, maxRounds, reason}`：UI 弹提示并可画分隔条。
- 自动轮的 nudge 文本**不作为用户消息存档**：user turn 带 `autoRound` +
  `system:true`，重载时渲染成 system 分隔条（"自动继续 · 第 N 轮"），
  不会和上一轮粘在一起。
- `session.list` 回 `autoRound`，会话列表在自动轮运行期间显示紫色"后台运行"点。
- 达到上限发 `auto.halted`。

## 3.2 本轮被截断时（不是错误）

两道上限触发时，本轮**是"暂停/切分"不是"失败"**：`loop.paused{reason, note, rounds}`
+ 把说明附在回答末尾。UI 只提示，不弹"已中断，可继续"——那会让一个安全上限
看起来像崩溃。

- **工具往返上限**（默认 200）：达到后**自动开新一轮**（`auto.next`），而不是停下
  等用户。新一轮的 nudge 明确写"这是系统自动切分，不是用户要求停止，也不代表任务
  完成，请先审视已完成的工作与当前进度再继续"——不说清楚，模型会把"被切分"当成
  "可以收尾了"，长任务就此提前结束。
- **连续相同调用**（默认 3）：直接停，因为这是模型卡住了，再切分也没用。
- 自动发出的 nudge 一律以 `SYSTEM:` 开头：它既不是用户输入，也不是模型自己的
  推理，前缀是唯一能区分三者的手段。

## 4. 挂起 / 唤醒

- 挂起：置位 `suspendRequested`，已发出 toolcall 跑完，不再发新调用；
  挂起中用户可改文件/改指令，恢复后 plan 不丢，从原处继续。
- **"已挂起"是真实状态，不是瞬时标志**：循环在工具批次边界真正停车时置
  `parked_ = true`（state 回到 idle）。`loop.state` 必须同时看 `suspend_requested()`
  **和** `parked()` 才报 `suspended`；只看前者的话循环一停标志就清了，UI 立刻退回
  "挂起"按钮，看起来像任务已经结束。
- **恢复必须真的跑**：`resume()` 只清请求位（对"还在跑、只是请求挂起"的循环够用）。
  一旦循环已经停车，没有任何线程会再看这个标志，所以 `resume_task()` 重跑
  `saved_messages_`；`AgentHost::resume` 在**锁外**调用它（同 `send()` 的约束：跑
  一整轮不能持宿主锁）。
- **挂起中发送消息 = 带引导继续**：`send()` 检测到 `parked_ && saved_valid_` 时走
  `continue_task(text)`，而不是开新轮——新轮会从磁盘历史重建，把停车时手里的工具
  结果整批丢掉。
- **中途引导的位置**：`apply_steering()` 在下一个模型调用前注入，同时把
  `executed_calls_.size()` 记进 `applied_steering_after_`（落盘为 `guidanceAfter`）。
  UI 按这个 tool anchor 把引导放在已经跑过的工具**之后**，而不是整轮最前面。
  队列有硬上限 `kMaxSteeringItems=20`，满了返回错误，不静默顶掉旧项。
- **窗口关闭 = 中止所有 agent**：`FsApi::abort_all_agents()` → `AgentHost::abort_all()`
  置 `shutting_down_` 并 interrupt 所有主 loop；子代理 loop 不在 `loops_` 里，靠
  `Options::external_cancel` 读到同一标志，模型调用/工具/摘要请求都会取消。
  `NativeBackend` 析构先退订事件、再 abort、再 join worker；否则关窗后进程会一直等
  正在跑的 turn（控制台不退出）。
- **停车不会发 `turn.done`**（那一轮没有正常结束）。UI 必须把
  `loop.state = suspended` 当作"本轮结束"来处理：最后一个 assistant 消息
  `streaming = false`、补 `ended_at`，一个字都没产出的空消息直接丢掉。少了这一步，
  转录里会永远转圈、计时器继续走——"停止生成/挂起做不完整"。
- **失败的一轮也必须落盘**（会话完整性）。用户看到的是：读文件编码出错 → 整轮死掉 →
  切页/重开什么都没了。两条规则：
  1. `run_loop` 是一个 try/catch 包着的 `run_loop_impl`。任何异常（最典型的是
     `nlohmann::json::dump()` 在非法 UTF-8 上抛 type_error.316）都会被 `fail_turn()`
     接住：把已流出的文本、已执行的 `executed_calls_`、`interim_texts_` 连同 `error`
     字段写成一个正式 assistant 轮，再 `loop.interrupted`。
  2. provider 失败的两个分支（首轮 / 后续轮）也调 `persist_failed_turn()`。以前这里
     只把 user 轮写盘，回答和工作过程全部丢弃。
  落盘的失败轮带 `failed: true` + `error`，重新打开时 UI 显示"本轮中断：…"。
- **所有进入 JSON 的字符串都必须是合法 UTF-8**：工具结果、终端输出、模型分片都可能
  不是。`dump_safe()`（fs_json.hpp）用 `error_handler_t::replace` 序列化，永不抛异常；
  文件读取一律先 `utf8::decode_to_utf8()`（BOM / UTF-16 / GBK / Latin-1 回退）。
- 唤醒源：用户输入 / 后台终端退出 / 模型定时唤醒 / 子代理完成。
- 模型自设唤醒：单次时长不设上限，用户可手动取消；
  项目级可见待唤醒状态，会话输入框区域显示待唤醒信息；
  "中断生成"与"取消待唤醒"共用按钮位、不同图标。
- 每次 wake 落库（turn.sysop + systemOps）。

## 5. 防过拟合（默认关）

- 检测器：归一化相似度/SimHash，阈值联调再定（起步建议：最近 3 次输出相似度>0.9 且重复≥3 次）。
- 触发后**后端不截断**，只在对应输出块下方给"截断/折叠"提示条（事件 `truncate.hint`），由用户点。
- 开关在会话设置。相似度函数自研小函数即可。

## 6. 子代理

- `spawn{task, model|inherit, cwd, permission≤父, timeout, depth+1}`。
- 只设**单会话并行上限**（默认 4，设置-子代理可调），不设全局上限；递归深度上限 3。
- 结果 summary 回父 + 全文进侧栏"子代理会话历史"可查；取消与超时落库。
- 预置 research、explore 等常用子代理，用户可为每个设开关/模型（设置-子代理）。
- **执行**：宿主 `run_subagent` 起一个独立 `AgentLoop`（不读父历史、不落库、
  只读代理只拿只读工具、权限不超过父），在 worker 线程跑完，
  `complete_with_summary` + `subagent.done` 事件回父。
- 模型通过 `subagent_spawn/list/cancel/steer/follow_up` 五个工具操作。

### 6.1 线程与锁（这里是真实的死锁来源，改动务必对照）

**规则：任何 join / 长跑都不能持 host 锁。**

1. **`reap_subagent_threads()` 必须在释放 host 锁之后调用。** 它 join 已完成的
   子代理线程，而子代理线程的最后一步是 `emit()`（要 host 锁）→ 定时器线程等线程、
   线程等锁。曾放在 `on_timer_tick()` 锁内，表现是"派完子代理几秒后整个应用冻住"。
2. **`subagent_follow_up` 不能持锁跑 agent loop。** 它跑的是完整 loop：会 emit（要锁）、
   还可能嵌套 spawn（worker 又要锁）。曾用 `lock_guard` 包住整段 —— **第二个子代理一出现
   就硬锁死**（一个线程持锁跑、另一个排队等锁）。现在：锁内只做快照
   （读 kind/parent/transcript/seed + `reopen_for_follow_up`），**释放后再跑**。
3. **`collect_runtime()` 必须持锁。** 它在 turn 线程跑，而子代理线程并发写
   `records_` / `todos_`。无锁读 `std::map` 是数据竞争：会 corrupt（崩）或在旋转里
   自旋（卡）。**这也是"第二个子代理才出现"的原因**——只有一个代理时没有并发写者。
4. **子代理收尾块**（`complete_with_summary` + `subagent_steps_` + `todo_op`）持锁。
5. **`spawn(wait=true)` 用轮询记录状态**，不 join（调用方是 turn 线程，join 会重现 1）。
6. **`invoke_tool` 的 `make_ctx` 持锁、工具本身不持锁**（慢工具不能挡住 suspend / 面板）。
7. **线程栈**：未 join 的 `std::thread` 仍占栈 → finished 的尽早 reap（spawn 前 + tick）。
8. **记录表要有上限**：`prune_finished(sid, keep=40)` 在 tick 里调用（按会话、只删已结束）。
   每条记录带整份转录，而转录里每条消息都含一份 system prompt —— 不剪就是每个会话
   几 MB 的增长，最后进程自己退出。

### 6.2 结束后继续（follow-up）

子代理报回后**不是终点**：`subagent_follow_up{id, text}` 用**同一个 id**、把**上一轮转录**
作为 `seed_messages` 回放，再跑一轮，同步返回新摘要。`steer` 只对运行中的有效（否则 busy）。

### 6.3 列表与详情

`subagent_list` **不带转录**（每项只给 `transcriptMessages` 条数）：转录里每条消息都
带一份 system prompt，3 个代理就把工具输出撑爆、最后一个条目被截断。转录走
`subagent_detail(id)`（工具无此入口，UI 用路由 `subagent.detail`）。

## 7. 规划树（PlanStore）

- 规划由模型指定：树形（目标→子目标→状态），存在后端，左悬浮规划栏展示。
- 模型用 `plan_update` / `plan_get` 维护；每 turn 产生 `planDelta` 落库；
  plan 未闭合是自动模式继续的依据。
- 规划栏同时显示 git 摘要（branch/提交态/更新时间，点击跳侧栏）。

## 8. 工作流

- `workflow_list` / `workflow_start` 让模型能用（此前 WorkflowStore 没有工具入口）。
- 一次性多步任务用 todo；流水线要反复跑或每步要单独重试/跳过/定向引导时才用工作流。

