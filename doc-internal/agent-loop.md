# Agent 主循环（agent-loop）

> 决策来源：addons §3、QA Q6–Q10。实现位置：`src/agent/`。

## 1. 状态机

```
idle ──send/append──▶ planning ⇄ toolCalling ──permission/ask──▶ waiting
  ▲                        │                       │
  │                        ▼                       ▼
  └──── autoNext ◀── turnDone ── suspendRequested（排队调用打完即停）
```

- `planning`：拼前缀（见 session-history.md §3）→ 调模型 → 得 plan/toolcalls。
- `toolCalling`：逐个过权限门（见 permissions.md）→ 执行 → 结果回填。
- `waiting`：等 permission 批准或 ask 回答；自动模式下 ask 不下发（见 §3）。
- `turnDone`：落库（full.json 追加、patch 存盘、用量记一行）→ 手动模式停，
  自动模式若 plan 未闭合（嵌套规划子目标未完成）自动 `autoNext`。

## 2. 手动 / 自动模式

| | 手动 | 自动 |
|---|---|---|
| turn 结束 | 停，等用户输入 | plan 未完成自动开下一轮 |
| 后台终端退出 | 徽标提示 | 自动唤醒继续 |
| 定时器到点 | 徽标提示 | 自动唤醒继续 |
| 子代理完成 | 徽标提示 | 自动唤醒继续 |
| ask | 弹 UI 等用户选 | 不下发 ask 工具 |

## 3. ask 语义

- 参数：`question + options[] + recommended（模型推荐/默认选项） + allowCustom`。
- 手动模式：UI 弹窗等用户选（可自定义输入），选择记 `ask.selection`。
- 自动模式：工具列表里直接拿掉 ask；防武断两层：
  1. 系统提示词写死（不确定不下定论）；
  2. 后端检测高风险动作（写文件/删文件/发网络/跑终端）且上下文不确定时
     自动降级为 `deferred`，UI 呈等待态等用户。
- 高风险动作表初版：写文件、删文件、发网络、跑终端。

## 4. 挂起 / 唤醒

- 挂起：置位 `suspendRequested`，已发出 toolcall 跑完，不再发新调用；
  挂起中用户可改文件/改指令，恢复后 plan 不丢，从原处继续。
- 唤醒源：用户输入 / 后台终端退出 / 模型定时唤醒 / 子代理完成。
- 模型自设唤醒：单次时长不设上限，用户可手动取消；
  项目级可见待唤醒状态，会话输入框区域显示待唤醒信息；
  “中断生成”与“取消待唤醒”共用按钮位、不同图标。
- 每次 wake 落库（turn.sysop + systemOps）。

## 5. 防过拟合（默认关）

- 检测器：归一化相似度/SimHash，阈值联调再定（起步建议：最近 3 次输出相似度>0.9 且重复≥3 次）。
- 触发后**后端不截断**，只在对应输出块下方给“截断/折叠”提示条（事件 `truncate.hint`），由用户点。
- 开关在会话设置。相似度函数自研小函数即可。

## 6. 子代理

- `spawn{task, model|inherit, cwd, permission≤父, timeout, depth+1}`。
- 只设**单会话并行上限**（默认 4，设置-子代理可调），不设全局上限；递归深度上限 3。
- 结果 summary 回父 + 全文进侧栏“子代理会话历史”可查；取消与超时落库。
- 预置 research、explore 等常用子代理，用户可为每个设开关/模型（设置-子代理）。

## 7. 规划树（PlanStore）

- 规划由模型指定：树形（目标→子目标→状态），存在后端，左悬浮规划栏展示。
- 每 turn 产生 `planDelta` 落库；plan 未闭合是自动模式继续的依据。
- 规划栏同时显示 git 摘要（branch/提交态/更新时间，点击跳侧栏）。
