# 会话历史（session-history）

> 决策来源：addons §2、QA Q1–Q4、Q6、Q9–Q10。目标：操作全记录、可复刻。

## 1. 目录布局

```
~/.flashsight/
  config.json                 # 全局设置
  providers.json              # 供应商 endpoint/key（前期明文，文件权限 600；TODO: 迁系统钥匙串）
  projects.json               # 项目注册表 {pid: {path, lastSession, lastActive}}
  usage/calls.jsonl           # 全局用量，见 usage.md

<project>/.flashsight/
  project.json                # 项目 MCP/skill 开关、权限覆盖、环境覆盖（含 ignoreGlobal）
  sessions/<sid>/
    full.json                 # 完整历史：唯一真相源，只追加不改写
    runtime.json              # 缓存：最近构建的运行时 history + 压缩记录（可删、可重建）
    patches/<turnId>.diff     # 每轮文件修改 diff（历史查看 + 回滚）
    terminals/<tid>.log[.gz]  # 终端日志（旧日志压缩，见 terminal.md）
```

## 1.2 历史分片（turns/）

`full.json` 曾**每追加一轮就整份重写**（读-改-写），于是每次追加是 O(n) 的 JSON 工作，
文件也无上限增长——实测一个只有 6 轮的会话就 90 KB（工具结果按全文存在 turn 里）。

现在目录是：

```
sessions/<sid>/
  full.json          # 只留 meta / sessionSettings / systemOps / snapshots（小而稳定）
  turns/000.json     # 第 0-15 轮
  turns/001.json     # 第 16-31 轮 …
  patches/ terminals/ runtime.json
```

- **每片 16 轮**（`kTurnsPerShard`）。每片几十 KB 量级；追加只读改写**最后一片** +
  那个小头文件，不再随历史长度变慢。
- `SessionFileStore::load_full()` 仍然返回**完整的 `turns` 数组**（读时把各片拼起来），
  所以上层（HistoryBuilder / UI / 回滚 / 分叉 / 遗忘）**完全不用改**。
- 所有会改写历史的路径都走 `save_full()`（把 turns 摊到分片、其余写头文件）；
  直接 `write_atomic(full.json)` 是错的——只写了头文件，`turns` 的改动被静默丢掉
  （`forget` 就这么错过一次）。
- `trim_turns` / `rollback` 之后会**删掉多余的分片**，否则被裁掉的轮次会在下次读时复活。
- **兼容旧格式**：老会话的 `turns` 内联在 full.json，读时原样返回；下一次追加时**自动迁移**
  （整份摊到分片，头文件不再带 turns）。
  判定依据是"**存在分片文件**"而不是"存在 turns/ 目录"——每个会话目录创建时就带一个空的
  turns/，按目录判断会把老文件误判成分片、静默丢掉全部历史（`wiring_legacy_full_json_still_reads`
  就是为抓这个写的）。
- 测试：`wiring_history_is_sharded_and_readable`（跨片读写、头文件不含 turns、回滚删多余分片）、
  `wiring_legacy_full_json_still_reads`（老格式可读 + 迁移后顺序不变）。

## 1.3 闪退安全：live 快照（runtime.json 的 live 字段）

一轮对话**只在结束时**才 append 到会话文件。进程崩了（WebView2 崩溃、断电），这一轮
就整体消失——用户看到的正是"闪退后上一轮内容丢失"。

`AgentLoop::maybe_persist_live()` 在**流式期间每 ~2 秒**把在飞的回合写到
`runtime.json` 的 `live` 字段：

```json
"live": {"running": true, "text": "已产生的回答…", "userText": "用户的问题",
         "startedAt": 0, "autoRound": 0}
```

- 节流 2 秒：快模型每秒上百个 delta，每次都写盘是纯浪费；2 秒远低于"用户能察觉的丢失"。
- **带上 `userText`**：问题是界面无法重建的那一半。
- 回合正常落地时写一次 `{"running": false}` **清空**它，否则下次打开会把已入库的回答
  当"崩溃恢复"再显示一遍。
- `session.liveTurn` 的语义变成两级：内存里在跑 → 原样返回；没在跑但盘上有 `running:true`
  的快照 → 返回 `{crashed:true, running:false, ...}`，UI 把它渲染成一条**已完成**的消息
  （`streaming:false`，没有实时计时），并提示"已恢复上次中断的内容"。
- 放在 `runtime.json` 是因为它本来就是"可删可重建的缓存"——在飞回合正好是这种生命周期。
- 测试：`loop_persists_live_snapshot_while_streaming`（流式期间有带正文与问题的快照、
  落地后最后一次写是清空）、`wiring_live_snapshot_survives_restart`（换一个 FsApi 进程
  仍能读回来，并标记为 crashed）。

## 2. full.json schema

```json
{
  "meta": {"sid": "", "pid": "", "cwd": "", "createdAt": 0, "title": ""},
  "sessionSettings": {
    "model": "", "thinking": "", "filePerm": "approve|permissive|full",
    "autoMode": false, "enterSend": "enter|ctrEnter"
  },
  "turns": [
    {
      "turnId": "", "time": 0, "role": "user|assistant",
      "modelSnapshot": {"model": "", "thinking": "", "configVersion": 0},
      "permissionSnapshot": {"filePerm": "", "locks": {}, "disabledTools": []},
      "promptRaw": "", "attachments": [{"kind": "image|file", "ref": ""}],
      "toolCalls": [{"name": "", "args": {}, "resultSummary": "", "resultRef": "",
                     "permissionDecision": "", "terminalRef": "", "subagentRef": "",
                     "truncatedHint": false}],
      "planDelta": {}, "usage": {"in": 0, "out": 0, "cache": 0, "cost": null},
      "streamSummary": "",
      "ask": {"question": "", "options": [], "recommended": 0, "selection": null},
      "sysop": null
    }
  ],
  "systemOps": [
    {"t": 0, "op": "setModel|setPermission|refreshConfig|compress|wake|suspend|resume|forget",
     "detail": {}}
  ],
  "snapshots": [{"turnId": "", "patchRef": ""}]
}
```

- `sessionSettings` 会话固化：切换会话即切换这组值；全局只存 provider 连接 + 新建默认值。
- `refreshConfig`：重载 provider/工具表/系统提示词，`configVersion+1`，新 turn 生效，
  清连续失败计数，不改历史，记一条 systemOps。
- `sysop` 字段记录本轮附带的系统事件（wake 来源、compress 区间、truncate hint 等）。

## 3. HistoryBuilder（full → 运行时 history）

- 输出**中性消息数组**（`{role, content}` / `{role:"assistant", toolCalls}` /
  `{role:"tool", toolCallId, content}`），请求前由 `provider/prompt_adapter` 渲染成
  当前端点的形态（`[OI]` / Anthropic / Responses）。**详细设计与理由见 `agent-context.md`。**
- 策略：**短视图（最近 N 轮全量）+ 外层压缩摘要**；N 与 token 预算双封顶
  （起步 N=20 + 预算 32000，阈值进设置）。预算从**最新**往回算，保证最新对话一定保留。
- 超预算时把窗口之前的轮次折叠成一条摘要消息（`prior_summary` + `keyPoints`），
  原文不再进 history；full.json 保留原文 + systemOps 记 `compress{from,to,byModel,summaryRef}`。
  `TODO`：目前是规则折叠，后续换成摘要模型。
- 提示词前缀装配顺序（定稿，省 token 版）：
  **前缀说明 → 环境 → 工具定义 → skills 与 MCP → 对话历史 → 配置 →
  新一轮对话**。实现上"配置"并入前缀的"环境"节；工具只在前缀里给一行索引，
  schema 走 API `tools` 参数。
- 切换模型/刷新配置不清空 history，新 turn 用新 `modelSnapshot`，老轮不改写。

## 4. 标题与折叠 summary

- 标题：先首句截断；当轮数 ≥3 或首轮模型自主 toolcall ≥10 次时调摘要模型升级标题；
  摘要模型默认继承会话模型（模型偏好里可另设）。
- 折叠 summary：每轮最后一次输出，提示词显式要求模型自带一段总结；
  已完成 chat 默认折叠只显示该总结（失败回退首句截断）。

## 5. 回滚 / 分叉 / 遗忘

- **回滚**：删除该 turn 及之后 turns，用 `patches/` 栈逆向还原文件
  （新文件删除记录、二进制/重命名对不上如实报错）；终端副作用不可回滚，UI 明示。
  不做每轮自动 git commit，不污染用户 git 历史。
- **分叉**：复制 full.json 前缀 + 新 sid，原会话不动。
- **遗忘**：运行时 history 剔除（全文/单条），full.json 标 `forgotten` 不物理删；
  压缩摘要需重建。
- 历史下“查看文件修改”：小按钮 → 该对话下展开 diff div（限高滚动、超长截断）→
  点击跳侧栏文件查看。
