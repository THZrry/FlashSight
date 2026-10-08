# 权限与文件锁（permissions）

> 决策来源：addons §4、§6；QA Q4–Q5、Q13。实现位置：`src/core/`（LockArbiter、SettingsStore）。

## 1. 文件三档权限（filePerm，会话固化）

| 档 | 含义 | 写行为 |
|---|---|---|
| `approve` | 每次询问 | 每次写/删/终端执行先弹批准 |
| `permissive` | 项目目录默认放行 | 项目内直接过，只拦项目外 + 黑名单 |
| `full` | 完整文件系统 | 只走 OS 语义，不设会话写锁 |

- 切换即时生效并记 systemOps；低→高需显式确认。
- **rm = 直接删除**，确认由批准门承担，不进回收站、不二次弹窗。

## 2. 会话写锁（LockArbiter）

- 非完整权限（approve/permissive）：同一项目同时只一个会话可写；
  其他会话写尝试返回 `lockedBy(session)` + UI 提示；锁随会话关闭/切换释放（超时兜底）。
- 完整权限（full）：遵循 OS 语义（已打开文件有锁→其余放行），不另设全局写锁。
- 读操作不受写锁影响。

## 3. 黑/白名单（settings-权限-文件）

- 优先级：**黑名单 > 白名单 > 三档默认**；命中拒绝要可解释（给模型机器可读原因）。
- 非 git 项目自动 `git init`：允许但默认关闭，开关放设置-权限-文件
  （动工时若发现放环境更顺可挪，记一条 systemOps 级别的文档更新）。

## 4. 工具拒绝协议

- 任一门拒绝（权限档/写锁/黑白名单）返回结构化拒绝：
  `{allowed:false, reason:"approve|lock|deny-list", lockedBy?, howToProceed}`，
  模型据此换道，不静默失败。
- 高风险动作（写/删/发网/跑终端）在自动模式不确定上下文下降级 `deferred`（见 agent-loop.md §3）。

## 5. 网络 / 终端权限门

- 网络：fetch/search 经 settings-权限-网络总开关 + 证书开关。
- 终端：settings-权限-终端总开关；shell 顺序 bash > zsh(linux替代) > pwsh7+ >
  PowerShell > cmd；启动全量探测静默降级（cmd 兜底），设置可显式指定。
- vision（截图）：屏/窗口/区域走权限门；读本地图走文件门。
