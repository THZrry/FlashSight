# 用量统计（usage）

> 决策来源：addons §7；QA Q3、Q23。实现：`src/core/UsageStore`。

## 1. 存储：JSONL

- `~/.flashsight/usage/calls.jsonl`，append-only，每行一次模型调用。
- 字段宽松（有什么记什么，读时容忍坏行/缺字段）：
  `ts / pid / sid / provider / model / in / out / cache / cost | quota | unknown /
  latency / toolCalls / error`。
- 字段不固定是故意的（额度信息各家差异大），yaml 嵌套以后需要再议。

## 2. 聚合

- 维度：provider × model × time。
- 用量页图表照 `temp/design.json` 落：12h/1d/7d/30d 柱状（按模型与时间）、
  扇形（模型占比）、token 分布表（输入/输出等）、成本分析表（钱/额度/无法统计三态）。
- 会话右键侧栏另有“用量统计及上下文窗口”小视图（当前会话 + 上下文水位）。

## 3. 保留与导出

- 默认保留 90 天（设置可改）；导出先做 CSV（`usage.exportCsv`）。
- 并发写：JSONL 追加写 + 读时容忍，可满足 v1；量大后再议索引/轮转（TODO）。

## 4. FsApi

`usage.query({provider?, model?, from?, to?})` / `usage.exportCsv(filter, path)`。
