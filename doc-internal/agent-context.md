# Agent 上下文设计（agent-context）

> 依据：`temp/agent_chat_structure.md`（用户给的上下文规定）+ `rules/goal.md` + `rules/addon1/2.md`，
> 参考 opencode 的分层做法（系统提示稳定前缀 + 会话历史 + 每轮注入 + 压缩摘要），
> 实现位置：`src/agent/agent_context.*`、`src/agent/history_builder.*`、
> `src/provider/prompt_adapter.*`、`src/agent/agent_loop.*`、`src/agent/agent_host.*`。
>
> 本文是**设计说明 + 现状记录**：写明的都是已实现并有单测覆盖的；未实现的标 `TODO`。

---

## 0. 三条硬约束（决定了所有结构）

| 约束 | 后果 |
|---|---|
| **前缀缓存** | 稳定内容必须逐字节不变地放在最前；任何易变字段（时间、规划、清单、水位）都不能进前缀，只能追加在最新一条用户消息里。 |
| **token 成本** | 工具 schema 走 API 的 `tools` 参数，**不再复制进提示词**；前缀只放"每个工具什么时候用"的一行索引（schema 表达不了这部分知识）。 |
| **跨 provider 可重放** | 会话里保存的是**中性消息格式**，请求前才按当前端点渲染成 `[OI]` / Anthropic / Responses 形态。切模型、换供应商都不会让历史变成非法请求。 |

这三条互相牵制：为了缓存要稳定，为了成本要精简，为了重放要把"形状"推迟到最后一刻。
所以上下文被拆成 **稳定前缀（prefix）**、**对话（conversation）**、**每轮状态块（runtime）** 三段。

---

## 1. 分层总览

```
┌─ 稳定前缀（system）—— 逐字节稳定，可命中缓存 ─────────────────┐
│ 1. 身份与角色        主代理 / 子代理（子代理额外带任务）      │
│ 2. 工作方式          结论先行、不谄媚、不编造、跟随代码风格   │
│ 3. 规划与工作流建议  ★ 显式写清什么时候用、什么时候不用      │
│ 4. 工具索引          name: 一行"何时用"，schema 不重复       │
│ 5. Skills / MCP      一行说明，细节用 load_skill 加载    │
│ 6. 环境              cwd/os/shell/编码/model/git/CLI/权限/模式│
│ 7. 项目约定          AGENTS.md 等（优先级高于通用习惯）       │
│ 8. 长期偏好          model.longMemory                         │
│ 9. 思考语言          仅当用户显式设置 zh/en 时写一行          │
└──────────────────────────────────────────────────────────────┘
┌─ 对话（conversation）—— 中性消息数组，历史累积 ──────────────┐
│ [压缩摘要] + user / assistant(toolCalls) / tool(result) ...   │
└──────────────────────────────────────────────────────────────┘
┌─ 每轮状态块（runtime）—— 追加在最新用户消息里 ───────────────┐
│ 时间 / 规划状态 / 本轮清单 / 子代理 / 待唤醒 / 上下文水位     │
└──────────────────────────────────────────────────────────────┘
```

顺序遵循 `doc-internal/session-history.md §3` 的定稿，只把"配置（模式、工具启用状况）"
并入了前缀第 6/4 节（它们本来就是稳定的）。

---

## 2. 稳定前缀：每一节写什么、为什么

### 2.1 身份与角色

主代理：`You are FlashSight, a coding agent running on the user's machine.` +
"直接操作用户的项目，做完再说，不要只说将会"。

子代理：`You are a subagent of FlashSight (kind).` + 任务原文 +
"只返回摘要（结论 + 文件:行号 + 风险），不复述过程、不建议下一步、不向用户提问"。

子代理前缀与主代理**不共用**：它们的职责边界不同，混用会让子代理写长篇报告
或反过来让主代理只给摘要。

### 2.2 工作方式（9 条，全部是行为约束）

结论先行 / 不谄媚 / 不编造（没读过不说内容，没跑过不说通过）/ 跟随代码风格 /
注释只写约束 / 每次工具调用都要推进任务 / 结束时给独立可读的总结。

这些是"模型默认不会做，但产品要求"的部分。**不写"你可以…"这种权限说明**——
权限由工具表和环境节表达。

### 2.3 规划与工作流建议 ★

用户明确要求的一段。分四块，每块都给"什么时候用"和**"什么时候不用"**：

| 块 | 内容要点 |
|---|---|
| **规划（todo）** | 3 步以上或要改多个文件 → `todo_add` 建清单，逐项 `todo_check`；清单是给用户看的进度条，不是形式；步骤会变就增删重排，不要为旧清单绕路；单步小事不建清单（清单本身也花 token）。 |
| **规划树（plan）** | 目标 + 子目标 + 状态，`plan_update` 维护；每轮结束前把状态改成真实状态；**未闭合的规划是自动模式继续的依据**。 |
| **子代理** | 判断标准是**上下文成本**：需要读很多文件才得出结论、而你只要结论时派；典型场景（全库找 bug / 找符号引用 / 审查一批文件 / 并行调研）；任务必须自包含（子代理看不到本会话）；只回摘要，要细节让它写文件；附上限（并行 4 / 深度 3，来自设置）。 |
| **工作流** | 一次性多步任务用 todo；**同一条流水线要反复跑、或每步要单独重试/跳过/定向引导时才用工作流**；失败只影响那一步。 |

自动模式下最后一块换成"ask 不可用"的替代策略：选可逆的那条、把假设写进回答、
需要用户拍板处停下等确认。

> 关键点：这些建议必须**和真实存在的工具**对应。此前项目有 PlanStore /
> SubagentManager / WorkflowStore，但工具表里一个都够不着，提示词等于在描述
> 模型没有的手。本次补了 `plan_update`、`subagent_spawn` 等 8 个工具
> （`src/tools/orchestration_tools.cpp`）。

### 2.4 工具索引

只列**当前真实存在**的工具，每条 `- name: 何时用`。禁用工具单列一行
`Disabled (do not call): ...`。schema 走 API `tools` 参数，前缀里不出现
`"parameters"`（有单测断言这一点）。

### 2.5 Skills / MCP

Skill 只给一行描述 + "要细节调用 load_skill"。MCP 包列出名字与启停状态。
未启用的 skill 不出现。

### 2.5.1 工具命名（一次真实的 400）

**OpenAI 与 Anthropic 都要求工具名匹配 `^[a-zA-Z0-9_-]{1,64}$`。**
本项目原来用 `file.read` / `plan.update` 这种点号命名，点号不合法，于是
**每一个请求**都被端点以 400 拒绝——模型根本跑不起来，而 UI 只显示 "http 400"
（响应体被丢掉了，见 §10.3）。

现在全部改成下划线风格，按"动作_对象"读起来自然：

| 旧 | 新 |
|---|---|
| `file.read` / `file.write` / `file.edit` | `read_file` / `write_file` / `edit_file` |
| `file.ls` / `file.find` / `file.rm` | `list_dir` / `find_files` / `remove_file` |
| `file.regex` / `dir.check` | `file_regex` / `check_dir` |
| `terminal.spawn/exec/kill/poll` | `terminal_spawn/exec/kill/poll` |
| `network.fetch` / `network.search` | `fetch_url` / `search_web` |
| `todo.add/list/check/clear` | `todo_add/list/check/clear` |
| `plan.update` / `plan.get` | `plan_update` / `plan_get` |
| `subagent.spawn/list/cancel/steer` | `subagent_spawn/list/cancel/steer` |
| `workflow.list/start` | `workflow_list` / `workflow_start` |
| `tool.skill.load` / `tool.skill.list` | `load_skill` / `list_skills` |
| `session.summarize` | `summarize_session` |
| `mcp.import/enable/disable/list` | `mcp_import/enable/disable/list` |
| `vision.screenshot` / `vision.readImage` | `screenshot` / `read_image` |

**两层防护**：

1. `ToolRegistry::register_tool` 调用 `wire_safe_tool_name()` 规范化名字
   （`.` `/` → `_`，超长截断），并把 `ToolDef.name` 一并改成规范化后的值——
   注册表键、发给模型的 schema、模型回调的名字三者永远一致。
   这样以后新加的工具（或导入的 MCP 工具）写错也不会让整个请求 400。
2. `tests/test_wire_names.cpp` 遍历真实注册表，断言每个名字合法、三种 provider
   形态渲染后依然合法。

注意：事件名（`subagent.done`）、FsApi 路由（`plan.update`）、设置键
（`history.compressAtPct`）**不是**工具名，它们不进请求体，保持点号。

### 2.6 环境

`cwd` / `os` / `shell` / 终端编码 / 模型 / 思考档 / git（branch + dirty + ahead/behind
+ 进行中的 merge/rebase）/ 可用 CLI 工具（name=path）/ 环境变量**仅名字**
（值可能是密钥）/ 文件权限档（用人话解释三档）/ 模式（手动=一轮即停，自动=规划未闭合继续）/
会话标题。

环境变量只给名字是刻意的：给值会把密钥写进每轮请求。

### 2.7 项目约定

按顺序找 `AGENTS.md` → `.flashsight/instructions.md` → `CLAUDE.md` → `.cursorrules`，
取第一个存在的，截断到 16 KB（按字符边界），并注明来源文件名。
明确写"优先级高于通用习惯"——否则模型会用自己的偏好覆盖项目规范。

### 2.8 长期偏好 / 2.9 思考语言

长期偏好来自设置 `model.longMemory`（用户已确认的偏好）。
思考语言**只在用户显式设了 zh/en 时写一行**；`auto` 时什么都不写——
文档里"仅在显式设置时加入"就是这个意思。

---

## 3. 对话：中性消息格式

```jsonc
{"role":"system",    "content":"..."}
{"role":"user",      "content":"..."}
{"role":"assistant", "content":"...", "toolCalls":[{"id","name","args":{}}]}
{"role":"tool",      "toolCallId":"...", "name":"...", "content":"...", "isError":false}
```

- `args` 在内部**永远是对象**，只在渲染时序列化成字符串。
- 一条 assistant 消息可以带多个 toolCall；每个结果单独一条 `tool` 消息，
  按 call id 对齐。**顺序固定**：assistant → 它的所有 tool 结果。
- 这是 `full.json` 的运行时形态，也是 `runtime.json` 可缓存的形态。

### 3.1 为什么不是直接拼 provider 形状

旧实现直接产出 Anthropic 形状（`{type:"tool_result"}` 塞进 user 消息）却发给所有
provider：`[OI]` 端点要么 400，要么静默忽略结果 → 模型看不到答案，**无限重发同一个
调用**。现在渲染集中在 `prompt::adapt(kind, messages, tools)`：

| 形态 | 消息 | 工具 | system |
|---|---|---|---|
| `[OI]` Chat / Custom | `role:"tool"` + `tool_call_id` | `{type:function, function:{...}}` | 在 messages 里 |
| Anthropic | user/assistant + `tool_use`/`tool_result` blocks | `{name, input_schema}` | **顶层 `system` 字段** |
| `[OI]` Response | `function_call` / `function_call_output` item | `{type:function, name, parameters}` | 在 input 里 |

适配层同时修两类结构性问题：
- Anthropic 要求**相邻同角色合并**、**首条必须是 user** → 合并 / 补 `[earlier context]` 引导；
- 工具参数 JSON 解析失败 → 打 `argsParseError`，**绝不**退化成空对象执行
  （否则"写文件"会变成"清空文件"）。

---

## 4. 每轮状态块（runtime）

追加在**最新一条用户消息**里，用 `<runtime>…</runtime>` 包住，原文放在 `<user>…</user>`：

```
<runtime>
time: 2026-10-22 09:31:00 UTC
plan state (an open plan keeps auto mode going):
- [done] 读 spec
- [running] 改实现
checklist (2 open):
[ ] 写单测
subagents:
- sub_1 (local-research): running
context: 42% (52000/128000 tokens)
</runtime>

<user>
（用户原文）
</user>
```

要点：

1. **不进前缀**，所以前缀缓存不会因为时间/规划变化而失效。
2. 写入 `full.json` 的是**原文**（`promptRaw`），不保存包装——重放历史时不会
   把上一轮的状态块当成用户说过的话。`AgentContext::unwrap_user_text` 负责回读。
3. 状态块内容由 `RuntimeState` 提供，`AgentHost::collect_runtime` 从 PlanStore /
   TodoStore / SubagentManager 收集；`AgentLoop` 自己不算业务状态。
4. 上下文水位用**上一次请求的 prompt tokens**（不是累计），窗口长度来自模型表；
   未知则不显示。

---

## 5. 历史构建与压缩

`HistoryBuilder::build_neutral(full.json)`：

1. 系统前缀（由 `AgentContext` 产出，每轮重算；历史里的旧 system 会被替换成新的）。
2. **压缩决策先做**（`should_compress`），两种触发：
   - **水位**（唯一自动触发）：上一次请求的 `prompt_tokens / context_limit ≥ compressAtPct`
     （默认 80%）。这是事实，不是估算——窗口快满了才动手，而不是按轮数。
   - **强制**：用户点"压缩"或 UI 在水位提示上点按钮（`session.compress`）。
   - 曾经还有两个触发，都**已删除**：`recentTurns`（比 N 轮更早的内容在**每一轮**都被折叠，
     与水位无关，长会话会持续压缩）和 `tokenBudget`（同样每轮生效的第二条隐形阈值）。
     provider 自己会处理长上下文；本地不需要一条与水位无关的常开压缩。
3. 一旦决定压缩，**必须真的折叠**：保留最近 `keepTurns`（默认 8，user+assistant
   各算一轮）条 turn 全量，其余压成一条摘要消息放在 system 之后。
   （早先的实现只是把预算调小，短会话仍放得下 → "点了压缩什么都没发生"。）
4. **未触发压缩时按时间顺序回放每一轮**：不做预算裁切，也没有"最近轮次窗口"。
5. 每个 turn 的回放：
   - user turn → `user` 消息（`promptRaw`；旧的 Anthropic 形态 turn 还会回放 `toolResults`）；
   - assistant turn → `assistant` 消息（`streamSummary` + `toolCalls`），随后**每个**
     调用一条 `tool` 消息（`detail` → `result` → `summary` 三级回退）。
     工具正文截断到 8000 字节（按字符边界），否则一次 200 KB 的读取会被每轮重发。

事件：压缩发生时发 `context.compressed{sid, from, to, droppedTurns, reason,
waterPct, promptTokens, contextLimit}`；想压但没东西可压发
`context.compressSkipped`。UI 侧栏显示水位（百分比 + 绝对值）、缓存命中、
本轮/会话成本，超过阈值时直接给"压缩上下文"按钮。

---

## 6. 主循环：边界与保护

状态机仍是 `idle → planning ⇄ toolCalling → waiting → turnDone`，但补了四道闸：

| 闸 | 行为 |
|---|---|
| **单轮工具往返上限**（60） | 达到就结束本轮，回答里注明"tool round limit reached"。 |
| **重复调用检测**（连续 3 次完全相同） | 停止并注明"the model repeated the same tool call"，不再烧预算。 |
| **自动续跑上限**（12） | 超过发 `auto.halted` 事件，停。 |
| **参数解析失败** | 不执行工具，回填 `invalid JSON` 错误，让模型重发。 |

**审批门**（`waiting`）现在按"整组"处理：一条 assistant 消息里的多个调用，
第 N 个需要批准时，把整组连同已产生的结果存进 `gate_messages_`；批准后执行它，
再继续执行**剩下的调用**，最后统一回填给模型。旧实现只记住一个 pending tool，
组里其余调用直接丢失（模型会以为它们成功了）。

**自动模式**：`turnDone` 时看规划/清单是否未闭合；未闭合就追加一条
`Continue…` 的 user 消息（带新的状态块）继续跑。续跑前**必须**把模型自己上一条
回答放进历史，否则看起来是"用户凭空说继续"。

---

## 7. 子代理上下文

- 独立 `AgentLoop`，`recent_turns = 1`、**不读父会话历史**、**不落库**。
- 继承父会话的**环境**（cwd/os/git/CLI/权限/模型），不继承对话。
- 工具集按预置类型裁剪：`local-research` 等只读代理**只拿只读工具**
  （读/查/搜索/skill/lsp/todo），写类工具一个都不给——能写的"研究代理"一定会去改代码。
- 权限 `min(父, 代理类型)`：只读代理额外钳到 permissive。
- `unattended = true`：需要批准的动作**直接拒绝并回填原因**，不会挂在
  `waiting` 等一个永远没人按的按钮。
- 运行在 worker 线程；完成时 `complete_with_summary` + `subagent.done` 事件，
  父会话自动模式据此唤醒。宿主析构时 join 所有线程（线程捕获 `this`）。
- 父上下文里只出现**摘要**（`summary`），全文进侧栏历史。

### 7.1 按步骤派发（complete-step）

用户要求的口径：**子代理要拆解单轮任务派出，每完成一步都单独用 step 那个子代理**，
这样工作过程可以显示但不进入主会话历史，省主会话上下文。

实现：

| 部分 | 做法 |
|---|---|
| 提示词 | 前缀的规划节明确写"清单建好后逐项 `subagent_spawn`（kind=complete-step，task 写验收标准，step 填标题，todoId 填 id）"，并写清依赖顺序（有依赖等摘要，无依赖可并行）。中英双语。 |
| 子代理上下文 | `subagent_step` 进前缀：`Checklist item: <标题>` + "只做这一项，做不了说明原因，不要扩大范围"。子代理不会被要求重新规划整个任务。 |
| 进度联动 | `subagent_steps_` 记录 `{step, todoId}`；完成时自动 `todo_check`，进度条不会和现实漂移。事件 `subagent.step` / `subagent.done` 都带 `step`。 |
| 上下文隔离 | 子代理的 `stream.delta` / `tool.*` 事件带 `sid=父会话` + `subagentId` + `fromSubagent`：主界面能显示"正在做什么"，但这些内容**不进 full.json**，也不进父会话的 messages。 |
| 只读代理 | `local-research` 等只读预置不能写；`complete-step` 可以写（它就是要改代码）。 |

---

## 8. 上下文窗口水位 / 压缩触发

- `context_limit` 由模型表提供（API 取回的 `context_len` 优先）。
- 水位 = 最近一次请求的 `prompt_tokens / context_limit`。这个值从**用量日志**读回
  （`usage.session.lastIn`），所以重开应用后水位仍然有效，不会"重新从 0 开始"。
- 压缩触发是**水位驱动**，且这是**唯一**的自动触发：`history.compressAtPct`
  （默认 80，可设 60/70/80/90/关闭）。轮数窗口与 token 硬上限已删除——它们每轮都生效，
  与水位无关。
- 压缩后保留 `history.keepTurns`（默认 8）条 turn 全量。
- 侧栏"用量"显示：水位百分比 + 绝对值（`tokens / window`）、缓存命中率、
  本轮 tokens/成本、会话累计成本、调用次数；超过阈值时直接给"压缩上下文"按钮。
  设置页只有两个旋钮：阈值档位与保留轮数（外加"立即压缩"）。
- `TODO`：摘要目前是规则折叠（首句 + keyPoints）；接摘要模型（`model.pref.summary`）
  生成摘要，并落 `systemOps: compress{from,to,byModel}`。

---

## 9. 编码与截断（一个真实的坑）

`json::dump()` 遇到非法 UTF-8 会**抛异常**，整轮直接失败。触发路径很隐蔽：

```cpp
desc.find_first_of("。.\n")   // '。' 是 E3 80 82，但 find_first_of 匹配的是"字节集合"
                              // 0x80/0x82 也是其他汉字的续字节 → 切出半个汉字
```

同理 `s.substr(0, 4000)` 也会切在字符中间。现在所有截断走
`src/common/fs_utf8.hpp`：`truncate()` 只切在字符边界，`sanitize()` 兜底清掉非法字节；
多字节分隔符一律用 `find("…")` 而不是 `find_first_of`。

---

## 10. 与会话刷新 / 新轮次的关系

| 场景 | 做法 |
|---|---|
| **新轮次（已压缩）** | 前缀重算（模型/工具/权限都可能变）+ 摘要作为第一条 user 消息 + 新对话。 |
| **会话刷新（refreshConfig）** | 不改历史，重算前缀与工具表，`configVersion+1`，记 systemOps。 |
| **切模型** | 不改历史；前缀里 model/思考档更新，历史仍是中性格式 → 直接可重放。 |
| **挂起 → 继续** | 保存的 `messages` 就是完整对话，续跑只是再调一次模型。 |
| **中断 → 继续** | `saved_messages_` + 用户引导，语义与"打断后续接"一致。 |

---

## 10.1 文件编辑协议（edit_file）

工具名与形态按用户要求做成 `edit_file` 那一类（**哈希编号 + 行号替换**），
不是文本块替换（旧的 `file.diffWrite` 已移除）。

```
read_file(path, offset?, limit?) -> {content, totalLines, startLine, endLine,
                                     eol, hash, truncated}
edit_file(path, hash?, lineEnding?, edits:[{start, end?, expect?, newText}])
        -> {changes:[{start,end,before,after}], hash, beforeHash, totalLines}
```

**两道校验，各自防一种错**：

| 校验 | 防的错误 | 失败时 |
|---|---|---|
| `hash`（整文件 xxh64 前 12 位） | 读到编辑之间文件被改过（并发保存、格式化、另一个 agent） | 拒绝并返回 `currentHash` + `stale:true`，提示重读 |
| `expect`（这些行的确切原文） | 行号对但内容不是模型以为的（过期读取、off-by-one） | 拒绝并返回 `actual`，让模型自己纠正 |

其它规则：

- 多个 edits **按原始行号解释**，从下往上应用，互不错位；区间重叠直接拒绝（顺序歧义）。
- `newText` 为空 = 删除这些行。
- 行号越界**拒绝**，不会静默追加到末尾。
- 保留文件原有的行尾（CRLF/LF）与**末尾换行状态**：编辑 `a\nb\n` 不会变成 `a\nb`，
  也不会把 CRLF 文件改成 LF。`lineEnding` 可显式覆盖。
- 成功返回新 `hash`，连续编辑不必重读。

**读取守卫**（`read_file`）：

| 条件 | 行为 |
|---|---|
| 文件 > 32 MB | 拒绝，提示用 find_files / file_regex / terminal |
| 文件 > 1 MB 且没给 offset/limit | 拒绝，要求显式分页 |
| 单行 > 4000 字符 | 该行截断并标 `longLinesTruncated` |
| 前 4 KB 含 NUL | 判二进制，拒绝（图片走 read_image） |

## 10.2 用量与成本的展示口径

`usage.session` 返回：

| 字段 | 含义 |
|---|---|
| `lastIn` / `contextWindow` | 水位分子/分母（都是 API 报的事实） |
| `cacheHit` / `cacheReported` | 缓存命中率；`-1` = 供应商从未上报 cache（**不是** 0%） |
| `totalCache` | 命中的 prompt tokens |
| `costKind` / `costKindByProvider` | money / quota / unknown，按 provider 分开 |
| `recent[]` | 最近 50 次调用的明细（UI 据此算"本轮"） |

成本三态必须分开显示：套餐内供应商上报 `cost: 0`，显示"¥0.00"是在说谎；
未知则显示"未知 · N 次"，而不是编一个数字。`cacheHit` 的分母只统计
**上报过 cache 字段**的行——把没上报的当成未命中会低估缓存效果。

侧栏"用量"显示：水位（百分比 + `tokens / window`）、缓存命中率（含命中 token 数）、
本轮 tokens/成本、会话累计成本、调用次数；超阈值时直接给"压缩上下文"按钮。

## 10.3 错误信息必须带原因
`provider_client` 原来把非 2xx 压成 `"http 400"`，响应体直接丢掉。结果是
**最难排查的一类问题**：端点明确说了 `tools[2].function.name: does not match
pattern '^[a-zA-Z0-9_-]+$'`，用户和开发者都只看到 "http 400"。

现在 `http_error_text()`：

1. 保留状态码；
2. 优先取 `{"error":{"message":...}}` / `{"error":"..."}` / `{"message":"..."}`
   里的结构化原因；
3. 非 JSON（代理的 HTML 错误页）就用原文；
4. 压成单行、截到 400 字符——它显示在 toast 里，不是日志窗口。

这条同样适用于 402/429（配额与限流），它们的文案本来就是给人看的。

## 10.4 图片与搜索：为什么"未接线"

两个工具原来返回硬编码的 not-supported，`search_web` 更是把 `{search:true}`
标记塞进 `net_fetch`——那个回调只读 `url`，所以每次都是 "empty url"。现在：

### 图片（`common/fs_image` + `common/fs_screen`）

| 层 | 做法 |
|---|---|
| 截图 | Windows：**Desktop Duplication（DXGI）优先，GDI BitBlt 兜底**。Duplication 读的是 DWM 合成后的表面，锁屏/未连接 RDP/多 GPU/HDR 下 BitBlt 会返回全黑而 Duplication 正常；Duplication 不可用时（无 GPU 适配器、被别的进程占用）才回落 GDI。macOS：CoreGraphics。Linux：X11 `XGetImage`；Wayland 明确报"不支持"（没有可移植截图 API）。`Capture::backend` 报告实际用了哪个（`dxgi`/`gdi`/`coregraphics`/`x11`）。 |
| 解码 | **stb_image**：PNG/JPEG/GIF/BMP/TGA/PSD/HDR 全部支持。WebP 没有 stb 解码器，按格式明确报错并给出真实尺寸。解码前先用 `stbi_info` 读头部，`kMaxPixels` 拦解压炸弹。 |
| 缩放 | **stb_image_resize2**（sRGB 感知，默认 Mitchell 类滤波）。截图里的文字缩到 1600px 仍然可读——朴素双线性做不到这点。只缩不放。 |
| 编码 | **stb_image_write** 出真压缩 PNG（此前自写的 stored-deflate 会把一张 1600px 截图撑到几 MB）。 |
| 模型能否"看见" | **关键**：工具返回 `dataUri`，loop 把它从 JSON 里取出来变成中性的 `parts` 块；adapter 再渲染成 [OI] `image_url` / Anthropic `image` base64 source。只返回文件路径的话，模型根本看不到图。 |

Windows 上 DWM 合成的桌面在非交互会话（锁屏、未连接的 RDP、服务上下文）里
**两个后端都可能全黑**——现在检测到全黑就报错并说明原因，而不是把一张黑图
喂给视觉模型让它自信地瞎猜。

### 搜索（`tools/search_backend`）

搜索没有通用协议，所以做成"可选后端 + 各自适配"：

| provider | 请求 | 响应 |
|---|---|---|
| `tavily` | POST `{query, max_results}` + `Bearer` | `results[]` |
| `brave` | GET `?q=&count=` + `X-Subscription-Token` | `web.results[]` |
| `searxng` | GET `?q=&format=json`（自建实例） | `results[]` |
| `custom` | POST `{query, count}` | 常见字段名兜底解析 |

设置页在"权限-网络"下，未配置时工具返回"去设置里配"而不是静默失败。
`url_encode` 用 `+` 表示空格、`+` 编码成 `%2B`——直接把 `+` 塞进查询串会被
服务端解码成空格。

### 桌面端卡死（`ui_native_backend`）

`NativeBackend` 继承了 `IUiBackend::call_async` 的默认实现，而默认实现就是
**同步调用**。于是桌面版每次 `session.send` 都在 UI 线程上跑完整个 turn：
窗口冻住、delta 全部排队、什么都不流式（浏览器版走 HttpBackend，天生异步，
所以从没暴露）。现在：

- 调用进队列，由**工作线程池**执行（按需增长到 4 个）。一个 turn 占住一个
  worker 好几分钟，UI 仍然要能打开面板、点停止。
- `FsApi` 不是线程安全的，所以 `UiApi::call` 在 UI→后端边界上取**一把递归锁**；
  `session.send` 与 `tool.invoke` 除外（它们会阻塞很久，自己只锁短前奏）。
- **嵌套调用内联执行**：turn 回调里再调后端（工具读设置）时，用 `thread_local`
  标记识别并直接执行；排队的话会等自己，必然死锁。
- 结果与事件共用一条队列，`poll()` 在 UI 线程交付，顺序不乱。
- `AgentLoop` 的 `state_` / `suspend_requested_` / `saved_valid_` /
  `compress_requested_` 改成 `atomic`：UI 线程每帧读它们画状态点。

---

## 10.5 工具结果进上下文只给摘要（投影）

工具结果落库时存的是**给人看的** `detail`（`result.dump(2)`，截到 4000 字符）。把它原样
回放给模型是长会话吃满窗口的主因：一次 `read_file` 的正文、一次 `edit_file` 的
before/after、一个 800 条的 `list_dir`，会在之后每一轮里重发一遍。

`HistoryBuilder::project_tool_result(name, result, detail)` 是**回放投影**：按工具保留
"能继续干活"的字段，丢掉 bulk。

| 工具 | 保留 | 丢掉 |
|---|---|---|
| `read_file` | path/startLine/endLine/totalLines/**hash**/eol/truncated + 正文前 900 字符 | 正文其余部分 |
| `edit_file` / `write_file` | path/totalLines/hash/beforeHash + changes 的**条数与行区间** | before/after 文本 |
| `list_dir` / `find_files` | 条数 + 名字样本（≤60 行）/命中样本（≤25 条） | 完整条目列表、CSV |
| 终端类 | exitCode/tid/status + **输出尾部** 1200 字符 | 开头部分（报错在尾部） |
| `screenshot` / `read_image` | 尺寸/格式/backend/path | dataUri（图片作为内容块只发一次） |
| `search_web` / `fetch_url` | 条数 + 标题/URL 样本、正文前 1200 字符 | 全文 |
| `todo_*` / `plan_*` / `subagent_*` | id/标题/done/状态 | 其余 |
| 未知（MCP/插件） | 结构化 JSON，逐层限长（字符串 400、数组 12 项） | 超出的部分 |
| **任何失败** | `error` / `errCode` / `howToProceed` / `denial` **永不丢** | — |

规则与兜底：

- **同一轮内不投影**：模型正要据此干活，原文要留（只做 16KB 上限）。投影只作用于
  **下一轮** 从盘上重建的历史。
- 老 turn 只有 `detail` 文本：先尝试 `Json::parse`，成功就按结构化投影；**失败则保留
  截断后的文本**（"既不是结构也不是 JSON"时把内容丢掉是最糟的结果）。
- 最后统一 `utf8::truncate(..., 4000)` 兜底，未知工具也不会撑爆。
- `hash` 必须留：它是 `edit_file` 的第二道校验，模型据此知道"我读到的是哪一版"。
  （`history_projects_tool_results_on_replay`）

## 10.6 阈值是设置，必须每轮重读

`history.compressAtPct` / `keepTurns` 原来只在
`loop_for()` 创建 loop 时读一次。用户改"设置-模型偏好-压缩"里的档位，**要重启才生效**——
这就是"处理阈值都是没有意义的选项"。

现在 `AgentHost::send` 每轮开跑前重读并 `loop->set_history_options()`。注意该 setter
**保留 `history.tools`**：工具表属于上下文刷新（`set_tools`），在阈值里一起覆盖会把
provider 的工具表清空。

## 10.7 子代理的模型从 loop 取（403 的根因）

`build_context(sid, ..., loop->context().model, ...)` 看起来对，其实在**新建的 loop** 上
`context()` 还是空的（`set_context` 才填）→ `ctx.model` 为空 → 存进 `session_ctx_` 的
seed 也是空 → 子代理 spawn 时模型名为空 → provider 路由"认不出模型就回退到第一个启用的
连接"（`fs_api.cpp` 的兜底）→ DeepSeek 的模型被发到 Anthropic 端点，**403**。

改成 `loop->model()`（新增访问器，读 `opts_.model`，也就是 `set_model` 写的那份）。
规则：**loop 的模型只有一个来源——`AgentLoop::model()`**；`ContextInput::model` 是
"这一次请求的描述"，不能当配置读。

## 11. 现状与待办

**已实现（有单测）**

- 稳定前缀 9 节 + 逐字节确定性（`context_prefix_is_deterministic`）。
- 规划/工作流/子代理建议进入前缀，中英双语（`context_planning_and_workflow_guidance_present`）。
- **"每步派一个 complete-step"** 写进前缀，step 子代理上下文只做一项
  （`main_context_recommends_step_delegation`、`subagent_context_is_step_scoped`）。
- 易变状态只进 runtime 块，原文可回读（`context_runtime_block_is_separate`）。
- 三形态适配 + 结构修复（`adapter_*`）。
- 多工具调用整组执行 / 审批中断续跑 / 参数错误不执行 / 重复调用熔断 /
  自动续跑保留上一条回答（`loop_*`）。
- 宿主接线：请求里真的有工具表与非空系统提示（`host_send_wires_tools_and_system_prompt`）。
- 历史预算从最新往回保留 + 折叠摘要 + 遗忘 + keyPoints（`history_*`）。
- **水位驱动压缩**：决策函数可测、压缩确实折叠、强制压缩、事件带原因
  （`compress_*`、`loop_emits_compressed_event_on_water_level`）。
- **edit_file 协议**：hash/expect 双校验、行号边界、重叠拒绝、CRLF 与末尾换行保留
  （`edit_*`）。
- **用量口径**：缓存命中率（未上报时为 -1）、按 provider 的成本类型
  （`usage_cache_*`、`usage_cost_kind_per_provider`）。
- 子代理执行器（独立上下文、只读裁剪、unattended 直接拒绝、线程 join）。
- **工具名合法**：37 个内置工具全部匹配 provider 的 `^[a-zA-Z0-9_-]{1,64}$`；
  注册层规范化兜底；三种 provider 形态渲染后仍合法
  （`tool_names_are_wire_safe`、`tool_registry_normalizes_illegal_names`、
  `wire_request_tools_are_legal`）。
- **错误信息带原因**：400/502 的响应体被解析并保留
  （`http_error_keeps_server_message`、`http_error_handles_non_json_body`、
  `http_error_anthropic_shape`）。
- **图片链路真接线**：PNG 编解码往返、坏数据拒绝、缩放、base64/data URI、
  `read_image` 返回 dataUri、`screenshot` 不再是 not-supported、电脑控制关闭时拒绝
  （`png_roundtrip`、`image_*`、`read_image_*`、`screenshot_*`）。
- **搜索真接线**：四种 provider 的请求形态、响应解析、缺 key 报错、空结果不算错、
  工具走 `web_search` 回调而不是 `net_fetch` 标记（`search_*`）。
- **桌面端不再卡死**：`call_async` 立即返回、结果经 `poll()` 交付、串行执行
  （`native_backend_call_async_is_async`、`native_backend_serializes_calls`、
  `default_call_async_runs_inline`）。

**TODO**

1. 摘要模型驱动的压缩（现在是规则折叠 + keyPoints）。
2. 图片/附件消息（`attachments` 目前只落库，未进请求）。
3. 工作流 runner 的推进仍在 `FsApi`（`start_next_pending_step`），
   与 agent 层的边界还可以再收一收。
4. `runtime.json` 缓存尚未写入（每次全量重建；对长会话是 O(n) 的 JSON 解析）。
5. 前缀分段缓存：目前每次重建整段字符串，可改成"分节缓存 + 拼装"，减少长会话开销。
6. `edit_file` 的 `expect` 目前按行区间整段比较；对超长区间（几百行）会让模型
   输出大量重复内容。可加 `expectFirst` / `expectLast`（首末行校验）作为低成本替代。
7. 成本单价表：现在只累加供应商上报的 cost。未上报的供应商可以按"模型单价 × tokens"
   估算，但必须标成估算值（与事实分开显示）。
