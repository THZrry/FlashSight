# Provider / 模型（provider）

> 决策来源：addons §7；QA Q2、Q15–Q16。实现位置：`src/provider/`。

## 1. 调用形态

| 形态 | 用途 |
|---|---|
| OpenAI Chat Completions | 通用 OpenAI 兼容 endpoint |
| OpenAI Response API | Response 形态 endpoint |
| Anthropic Messages | Anthropic 形态 |
| 自定义 | endpoint + 自定义 header |
| 套餐连接 | 额度类（如 x-opencode-session 形态），单独一类 |

- v1 只保证“能连、能计次数”；额度余额显示能做多少做多少，不阻塞主流程。
- provider 层预留 `quota` 字段与三态显示（钱 / 额度 / unknown），用量页成本表格依赖它。
- TODO：调研各家套餐需要的功能（含 opencode 内置项），后面直接用使用字段定义多家。

## 2. 密钥存放

- `~/.flashsight/providers.json`，前期明文，文件权限 600。
- TODO：迁系统钥匙串（Win Credential Manager / Linux Secret Service），config 只存引用。

## 3. 模型默认值（四级回退）

用户自定义 > API 获取（一键获取模型列表）> 预置列表（`models-preset.json`）> 保守缺省。
一键获取失败回退上一级并提示，不清空已有配置。

## 4. 模型能力标签

`vision / thinking / toolUse / contextLen`：预置表维护 + API 获取后自动填。

- **不支持 toolUse 的模型拒绝进会话**（创建时硬拒绝，UI 明示）。
- 供应商虚标能力导致运行时报错：原样透出，不归工具层兜底。

## 5. 模型相关设置面（设置-模型）

- 供应商设置：三形态 + 自定义 header + 套餐；常见模型名默认设置；一键获取列表。
- 模型偏好：子代理/摘要模型的通用偏好（摘要默认继承会话模型，可另设）。
- MCP 与 Skill：本地工具管理，可从外部文件夹导入（内置 claude code、open code 等路径配置）。
- 子代理：预置 research、explore 等常用代理，各设开关/模型。
- 插件：v1 灰色禁用，不实现。钩子：语义 TBD。
- 长期记忆：用户偏好管理（设置-模型-长期记忆）。
