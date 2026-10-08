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

- 输出 provider 消息数组。策略：**短视图（最近 N 轮全量）+ 外层压缩摘要**；
  N 与 token 预算双封顶（起步 N=20 + token 上限，阈值进设置）。
- 超预算时对最旧若干轮调摘要模型生成 summary，原文不再进 history；
  full.json 保留原文 + systemOps 记 `compress{from,to,byModel,summaryRef}`。
- 提示词前缀装配顺序（定稿，省 token 版）：
  **前缀说明 → 环境 → 工具定义 → skills 与 MCP → 对话历史 → 配置 →
  新一轮对话**。其中“配置·工具启用状况”只写两类：
  被禁用的系统工具、在运行期间启用状态变化了的自定义工具。
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
