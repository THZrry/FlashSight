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

## 2.1 成本的四个来源（按权威度）

| 顺序 | 来源 | `cost_kind` | UI 显示 |
|---|---|---|---|
| 1 | 端点回报 `cost` 字段（opencode/Zen）或 `usage.cost`（OpenRouter） | `money` | `¥1.23` |
| 2 | **模型单价 × token**（端点没报钱时） | `estimated` | `≈¥1.23（估算）` |
| 3 | 套餐/额度连接（端点报 `cost:0`） | `quota` | `套餐内` |
| 4 | 单价未知 | `unknown` | `未知` |

`estimated` 必须显式标注：估算值看起来像账单，用户会拿去对账。
单价在 `ModelCapabilities.price_{in,out,cache}_per_m`（每百万 token），
预置表带常见模型的价格，用户可在模型设置里覆盖。

缓存字段兼容三种写法：`prompt_tokens_cached`、`prompt_cache_hit_tokens`
（DeepSeek）、`prompt_tokens_details.cached_tokens`（OpenRouter/[OI] 新形态）。
漏读它会让缓存命中率显示 0%，并把缓存 token 按全价计费。

## 3. 保留与导出

- 默认保留 90 天（设置可改）；导出先做 CSV（`usage.exportCsv`）。
- 并发写：JSONL 追加写 + 读时容忍，可满足 v1；量大后再议索引/轮转（TODO）。

## 4. FsApi

`usage.query({provider?, model?, from?, to?})` / `usage.exportCsv(filter, path)`。
