# P1 AppCore（应用核心）

> 前置：P0 全局。产出：无模型、无 UI 也能建会话、读写历史、调通 FsApi 骨架。
> 实现位置：`src/storage` + `src/core`。单测先行（见 build.md §4）。

## 存储层（src/storage）

- [ ] 路径解析：`~/.flashsight/`（Win `%USERPROFILE%/.flashsight`）与 `<project>/.flashsight/`。
- [ ] 全局文件：`config.json` / `providers.json`（权限 600，前期明文；TODO 钥匙串）/
  `projects.json` 注册表 / `usage/calls.jsonl`（append-only，坏行容忍，见 usage.md）。
- [ ] 项目文件：`project.json`（MCP开关/权限覆盖/环境覆盖/ignoreGlobal）读写。
- [ ] 会话文件：`full.json`（只追加）/ `runtime.json`（可重建）/ `patches/<turnId>.diff` /
  `terminals/<tid>.log[.gz]` 读写；缺 `terminals` 日志不报错。
- [ ] JSONL 读写器：追加写 + 流式读 + 坏行跳过计数。

## SettingsStore（全局+项目两级）

- [ ] get/set（带来源标注：全局/项目；冲突项目优先，可 ignoreGlobal）。
- [ ] 新建会话默认值（模型/思考强度/权限/自动挡/发送键）管理。
- [ ] shell 显式指定 + 探测结果缓存（见 terminal.md §4，全量探测静默降级）。
- [ ] 环境变量与 CLI 工具 key-value（全局+项目，设置页显示来源）。
- [ ] 非 git 项目自动 `git init` 开关（默认关，位置：设置-权限-文件）。

## ProjectManager

- [ ] `project.list/open/create/remove/get`（FsApi `project.*`）。
- [ ] 注册表维护（路径→pid，名/最后会话/最后活跃）。
- [ ] 新建文件夹 / 打开文件夹；远程连接 v1 只留入口（libssh2 预留）。

## SessionManager

- [ ] `session.new/open/close`（会话固化 `model/thinking/filePerm/autoMode/enterSend`）。
- [ ] 无 toolUse 模型硬拒绝进会话（含 UI 明示错误码）。
- [ ] 切换会话即切换整组会话设置；`setModel/setThinking/setPermission/setAutoMode/setEnterSend` 即时生效+记 systemOps。
- [ ] 低→高权限切换显式确认流程（后端给确认令牌，UI 弹确认）。
- [ ] `refreshConfig`：重载 provider/工具表/系统提示词，`configVersion+1`，清连续失败计数，记 systemOps。
- [ ] 标题：首句截断先行（摘要模型升级留到 P3）。

## turn 落库 / 回滚 / 分叉 / 遗忘

- [ ] turn 追加写 full.json（含 modelSnapshot/permissionSnapshot/toolCalls/usage/ask/sysop）。
- [ ] patch 栈：每轮文件修改存 `patches/<turnId>.diff` + 新文件删除记录 + snapshots 指针。
- [ ] `rollback(sid,turnId)`：删后续 turns + patch 逆向还原；二进制/重命名对不上如实报错；
  终端副作用不可回滚（返回里注明，UI 明示）。
- [ ] `fork(sid,turnId)`：复制前缀 + 新 sid，原会话不动。
- [ ] `forget(sid,scope)`：全文/单条；full 标 `forgotten` 不物理删；压缩摘要重建（P3 接 HistoryBuilder）。
- [ ] git 侧栏独立工具：历史/分支查看、checkout/reset（调 git CLI，与 patch 回滚互不干扰）。

## PlanStore

- [ ] 规划树 CRUD（目标→子目标→状态），`plan.get/update`。
- [ ] 每 turn `planDelta` 落库。

## UsageStore

- [ ] 调用记录写 JSONL（一行一次，宽字段，见 usage.md §1）。
- [ ] `usage.query({provider?,model?,from?,to?})` 聚合（provider×model×time）。
- [ ] `usage.exportCsv`；90 天保留（设置可改，清理任务）。

## LockArbiter（文件锁）

- [ ] 非完整权限同项目单写锁：申请/释放/查询持有者；关闭/切换会话释放；超时兜底。
- [ ] 拒绝返回 `lockedBy(session)` + UI 事件 `lock.change`。
- [ ] 完整权限：不设全局锁（走 OS 语义）。
- [ ] 读操作不受锁影响。

## TerminalRegistry

- [ ] 注册表 CRUD（tid 自增、kind、shell、cwd、编码、状态、退出码、时长、logRef、wakeOnExit）。
- [ ] 统一编号（用户终端与模型终端同一序列）。
- [ ] 日志：写 `.log`，超 10 轮旧日志压 `.gz`，用户可删，缺文件不报错。
- [ ] 编码：默认系统编码，可切换（`terminal.setEncoding`）。
- [ ] 实际 spawn/kill/poll 在 P3 工具层实现，本阶段先注册表 + 桩。

## 黑白名单

- [ ] 名单 CRUD + 匹配（黑 > 白 > 三档默认），拒绝可解释（reason 机读）。

## FsApi 骨架

- [ ] P1 范围方法先跑通（project/session/terminal-list/usage/settings/plan），无模型桩返回固定串。
- [ ] 错误码全覆盖单测（拒绝三类：approve/lock/deny-list）。

## 验收（M1）

- [ ] 建空会话→写几轮→读回 full.json→回滚→分叉→遗忘，全单测通过，无 UI 无模型。
