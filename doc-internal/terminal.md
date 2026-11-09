# 终端子系统（terminal）

> 决策来源：addons §8；QA Q11–Q13。实现：`src/core/TerminalRegistry` + `src/tools/terminal`，
> 子进程管理用 reproc。

## 1. 两类终端

| 类 | 创建者 | 交互 | 生命周期 |
|---|---|---|---|
| 用户终端 | 用户手动新建 | 可交互（bash/pwsh 等） | 用户手动停止/关闭 |
| 模型终端 | 模型 tool 调用 | 前台 exec（工具强制 60s 预算）/ 后台 spawn | 前台到预算返回 timedOut 并转后台（不杀）；后台退出触发 wake/徽标 |

- 编号统一自增（用户建的与模型建的同一序列）。
- 前台到预算时：`terminal_exec` 立即返回 `status:"running"` + `timedOut:true`
  + 已产生的输出；命令继续作为 model-bg 运行，模型自己决定 poll / kill / 继续等。
  记录（含 `timedOut`）必须在“所有终端”里可见，原 history 保留 `terminalRef` 引用。
- `timeoutMs` 只能缩短前台等待，默认/上限都是 60000ms；长任务用 `terminal_spawn`。
- **退出检测不能用 `reproc_wait(handle, 0)`**：reproc 的 Windows 实现只要 exit
  pipe 可读就会进 `WaitForSingleObject(INFINITE)`；BusyBox 起子进程时会提前关闭
  继承的 exit pipe 句柄，于是前台 60s 预算和 `tick()` 都会永久卡住。管理器自己
  持有 `OpenProcess(SYNCHRONIZE)` 句柄（POSIX 用 `waitpid(WNOHANG)`）做真正
  非阻塞的 `try_wait`；`spawn/poll/tick/kill` 都不再调用 reproc_wait。
- **锁顺序**：`tick()` 持有终端 `mu_`；UI 的 `terminal.grid/poll/kill` 不再持有
  全局后端锁去等 `mu_`（这些路由已从 UiApi 的全局锁排除）。`TerminalRegistry`
  没有自己的锁，所有读都通过 `TerminalManager::entry_snapshot /
  entries_for_session` 在 `mu_` 下拷出副本。

## 2. 注册表字段

`{tid, sid, kind:user|model-fg|model-bg, shell, cwd, encoding, status:running|exited|killed,
exitCode, startedAt, endedAt, logPath, wakeOnExit, timedOut}`。

- 编码默认系统编码，可切换。
- 终端页：集合页 + 多编号页；手动停止按钮常驻。

## 3. 日志保留

- `sessions/<sid>/terminals/<tid>.log`，记录退出码+时长+输出。
- 超 10 轮前的旧日志自动压缩（`.log.gz`）；默认不删除，用户可手动删；
  **文件缺失不得报错**（graceful 降级，读到缺失返回空 + 提示）。
- 磁盘压力总封顶以后再议（TODO）。

## 4. shell 探测

- 启动时全量探测，按 bash > zsh > pwsh7+ > PowerShell > cmd 选最优；
  cmd 必可用，静默降级不弹窗（只记日志）；用户可在设置显式指定 shell；
  终端页显示当前 shell。

## 5. 环境注入

- 全局环境变量 + 项目 `project.json` 覆盖（可设 `ignoreGlobal`）；
- 终端 spawn 时合并注入；设置页每条显示来源（全局/项目）。
- CLI 工具路径 key-value（预置常见 key + 用户自探）：运行时把 key 告诉模型，模型按 key 调用。
