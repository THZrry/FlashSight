# P3 Agent 引擎（核心流程 + 内置工具）

> 前置：P1 + P2。产出：手动模式端到端；自动/挂起/唤醒/回滚/子代理单测全过（M3）。
> 实现位置：`src/agent/` + `src/tools/`。这是功能最密的一层，按小节逐项勾。

## HistoryBuilder

- [x] 短视图（最近 N 轮全量）+ token 预算双封顶（起步 N=20，阈值进设置）。
- [x] 超预算压缩：最旧若干轮调摘要模型生成 summary；full 保留原文；
  systemOps 记 `compress{from,to,byModel,summaryRef}`；`runtime.json` 缓存。
- [x] 前缀装配顺序：前缀说明→环境→工具定义→skills与MCP→对话历史→配置→新一轮对话；
  工具启用状况只写“被禁用的系统工具 + 运行期变化的自定义工具”。
- [x] 遗忘后压缩摘要重建；切换模型/刷新配置不清空 history。
- [x] 标题升级：≥3 轮或首轮 toolcall≥10 次调摘要模型（默认继承会话模型，可另设）；
  折叠 summary 靠提示词让模型自带总结（失败回退首句截断）。

## Loop 状态机

- [x] planning ⇄ toolCalling ⇄ waiting（permission/ask）→ turnDone → idle/autoNext。
- [x] 手动模式：一轮结束即停，等输入。
- [x] 自动模式：plan 未闭合自动开下一轮；后台终端退出/定时器/子代理完成自动唤醒。
- [x] 连续失败计数 + refreshConfig 清零（P1 已记，这里接行为）。

## 挂起 / 唤醒（Suspender / WakeScheduler）

- [x] `suspend`：置位，已发出 toolcall 跑完，不再发新调用；plan 不丢；挂起中可改文件/改指令。
- [x] `resume`：从原处继续。
- [x] 唤醒源四件套：用户输入 / 后台终端退出 / 模型定时（单次时长不设上限，用户可取消；
  项目级+输入框区双处可见）/ 子代理完成。
- [x] `cancelWake`；中断生成与取消待唤醒共用按钮位（UI 在 P4，事件先行：`wake.fired`）。
- [x] 每次 wake 落库（turn.sysop + systemOps）。

## ask

- [x] 工具参数 `question/options[]/recommended/allowCustom`；手动模式 `ask.prompt` 等用户选。
- [x] 自动模式工具列表拿掉 ask；防武断双层：提示词写死 + 高风险动作
  （写/删/发网/跑终端）不确定上下文下降级 `deferred`。
- [x] `tool.ask.answer(sid,askId,selection)` 用户回答链路。

## 防过拟合（默认关）

- [x] 检测器（自研 SimHash/归一化相似度，阈值联调再定）。
- [x] 触发只发 `truncate.hint`，在输出块下加提示条；后端不截断、不杀 turn。
- [x] 开关在会话设置。

## 子代理

- [x] `subagent_spawn{task,model|inherit,cwd,permission≤父,timeout,depth+1}`；
  单会话并行上限默认 4（设置可调），深度上限 3；取消与超时落库。
- [x] 结果 summary 回父 + 全文进侧栏历史（事件 `subagent.done`）。
- [x] 预置 research、explore 等常用代理，各设开关/模型（设置-子代理）。

## 内置工具（src/tools，逐个接，每接一个单测+权限门联调）

- [x] read_file（offset/limit + 二进制守卫）/ ls / find-grep。
- [x] write_file / diffWrite（dtl；上下文对不上直接失败；diff 进 patches/）。
- [x] remove_file（直接删除；确认走批准门；删前快照保证可回滚）。
- [x] terminal_exec/spawn/kill/poll（reproc；前台超 5min 自动转后台并通知+事件；
  后台退出→wake 或徽标；输出流式 `terminal.output/exit`）。
- [x] fetch_url（method/headers/timeout/sizeCap/证书开关；脱敏 trace）。
- [ ] search_web（先调研开源实现，有则用，无则接可配 HTTP 搜索 API）。
- [x] screenshot（屏/窗口/区域，权限门）/ readImage（最大边+质量压缩可设）。
- [x] MCP 与 Skill：本地工具管理，从外部文件夹导入（claude code、open code 等路径预置）；
  自定义工具启用状态变化进前缀（见 HistoryBuilder）。
- [x] 长期记忆：用户偏好读写（设置-模型-长期记忆），前缀按需注入（策略实现时定）。

## 验收（M3）

- [x] 手动模式端到端：send→plan→toolcall（文件+终端至少各一）→落库→回滚还原→分叉→遗忘。
- [x] 自动/挂起/唤醒/ask/subagent/过拟合提示，单测全过（时钟注入，不睡真 5 分钟）。
