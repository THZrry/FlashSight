# P2 Provider（模型接入）

> 前置：P1 存储就绪（用量边写边用）。产出：任一形态调通真实模型，用量记一行。
> 实现位置：`src/provider/`。网络可桩，单测不碰真网；联调用真 key 手动验。

## 调用形态（逐个接，每接一个都要单测+真机联调）

- [x] OpenAI Chat Completions（含自定义 endpoint + 自定义 header）。
- [x] OpenAI Response API。
- [x] Anthropic Messages。
- [x] 套餐连接（额度类，如 x-opencode-session 形态）：v1 只保“能连、能计次数”，
  余额显示能做多少做多少；`quota` 字段与三态（钱/额度/unknown）先行。
- [ ] TODO：调研各家套餐需要的功能（含 opencode 内置项），后面直接用使用字段定义多家。

## 密钥与连接

- [x] `providers.json` 读写（前期明文 600 权限）；连接增删改查 `provider.listProviders`。
- [x] `testConnection(providerId)`：连通性 + 延迟 + 能力探测回填。
- [ ] TODO：迁系统钥匙串（Win Credential Manager / Linux Secret Service）。

## 模型表与能力

- [x] `listModels(providerId)`：一键获取；失败回退上一级并提示，不清空已有配置。
- [x] 预置表 `models-preset.json`（用户自定义 > API 获取 > 预置 > 保守缺省）。
- [x] 能力标签 `vision/thinking/toolUse/contextLen`：预置 + API 获取后自动填。
- [x] 供应商虚标能力：运行时报错原样透出，不兜底（单测覆盖透出路径）。

## 流式与用量

- [x] 流式 delta 经 `stream.delta` 事件推送（本阶段先推给测试订阅者，UI 在 P4 接）。
- [x] 每次调用记用量（一行 JSONL：in/out/cache/cost|quota|unknown/latency/toolCalls/error）。
- [x] 超时/重试/失败计数（给 refreshConfig 清计数用）策略定死并单测。

## 验收（M2）

- [x] 任一形态真模型走通一次 `send→stream→turnDone→用量行`，全链落库。
