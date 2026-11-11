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

## 3.1 会话记住的模型可能失效

会话保存它上次使用的模型（`sessionSettings.model`），但那个模型随时可能不可用：
用户禁用了它，或者整个供应商被删了。此时**不能**让模型框继续显示一个死名字：

- 打开会话 / 切换标签时，前端调 `model.allEnabled` 核对一次。
- 记着的模型不在列表里 → 清空模型框（显示"选择模型"占位）、把按钮标成警告色、
  **禁用发送按钮**，并提示"上次使用的模型已不可用，请重新选择"。
- `send_message()` 里有同一道闸（按钮被绕过、或从键盘触发时也拦得住）。
- 显式选一个模型后立刻恢复可发。

为什么要在 UI 层拦：真发出去会失败在 provider 层，错误信息（HTTP 状态 / 网络错误）
里根本不含"模型不存在"，用户只会以为是网络问题。无后端时（UI 测试 / 纯预览）
跳过这道检查，否则演示路径就死了。

## 4. 模型能力标签

`vision / thinking / toolUse / contextLen`：预置表维护 + API 获取后自动填。

**一键获取要把端点上能问到的都填进来**，用户不该再手填一遍。已解析的字段：

| 端点字段 | 落到 |
|---|---|
| `name` | display（模型显示名） |
| `context_length` / `context_window` / `max_context_length` / `max_model_len`（含 `top_provider` 嵌套） | `caps.context_len` |
| `max_output_tokens` / `max_completion_tokens` | `caps.max_output` |
| `input_modalities: [text,image,...]`（或 `architecture.input_modalities`） | `caps.vision/audio/video/pdf` |
| `effort.supported_levels` / `effort.default_level` | `thinking.levels` / `default_level`（模型自己的档位表，优先于按名字粗猜） |
| `capabilities.tools` / `tool_use` / `structured_output` / `vision` | `caps.tool_use` / `structured_output` / `vision` |

- 取回的结果**会落盘**（`provider.apiModels`）：上下文长度这类事实只在这里有，
  只放内存的话重启后 `resolve()` 拿不到，上下文水位就静悄悄地消失。
- `resolve()` 合并而不是覆盖：用户只改了 `enabled` 的那条仍要继承 API 的
  `context_len` / `max_output` / 档位，否则"用户碰过的模型"水位会变成 0。
- 上下文水位的百分比是 `上次请求的 prompt tokens / context_len`，且**最低显示 1%**：
  1M 窗口下几千 token 会整除成 0，看起来和"没算出来"一样。
- **不支持 toolUse 的模型拒绝进会话**（创建时硬拒绝，UI 明示）。
- 供应商虚标能力导致运行时报错：原样透出，不归工具层兜底。

### 4.1 思考档位：三处必须一致

档位（`thinking.levels`）从"模型表"流到"请求体"要经过三段，任一段漏掉都会表现为
"设了档位但没效果"或者"控件根本不出现"：

1. **来源**：`ModelEntry.thinking.levels`。优先用用户/API 配置的值；只有为空时才
   由 `thinking_levels_for(id)` 按名字粗猜（o1/claude/gemini/deepseek-r/qwen3…）。
2. **UI 渲染**：输入区的控件只在 `model_thinking_levels` 非空时出现。**一个
   `caps.thinking=false` 且档位表为空的模型（例如用户自定义的 `deepseek-flash`）
   会让控件永远不出现** —— 这曾经被误判成 CSS/宽度问题。现在档位表为空且已选模型时
   回退成 `disable` / `enable` 两档；默认项是"默认"（= 发空值），所以渲染本身不会
   改变请求，只有用户明确选"开"才发提示。
3. **出网**：`wire_thinking()` 是**唯一**把档位写进请求体的地方，负责归一化：
   `disable/off/disabled/none` → **整个字段省略**；`enable/on/enabled` → `medium`；
   其余原样。没有哪个厂商接受字面上的 `enable`（OpenAI 只认
   minimal/low/medium/high；Anthropic 没有命名档位；其他家把未知 effort 当 400）。
   - OpenAI Chat：`reasoning_effort`
   - OpenAI Responses：`reasoning.effort`（此前**完全没处理**）
   - Anthropic：`thinking.budget_tokens`，minimal 1024 / low 2048 / medium 4096 /
     high 8192 / max 16384，且必须小于 `max_tokens`

测试：`provider_thinking_effort_normalised`（三态归一化）、
`provider_thinking_anthropic_budget`（预算与省略）、
`uiapp_composer_thinking_falls_back_to_disable_enable`、
`uiapp_composer_thinking_prefers_configured_levels`。

## 5. 模型相关设置面（设置-模型）

- 供应商设置：三形态 + 自定义 header + 套餐；常见模型名默认设置；一键获取列表。
- 模型偏好：子代理/摘要模型的通用偏好（摘要默认继承会话模型，可另设）。
- MCP 与 Skill：本地工具管理，可从外部文件夹导入（内置 claude code、open code 等路径配置）。
- 子代理：预置 research、explore 等常用代理，各设开关/模型。
- 插件：v1 灰色禁用，不实现。钩子：语义 TBD。
- 长期记忆：用户偏好管理（设置-模型-长期记忆）。

## 6. 图片输入（read_image / screenshot）

工具结果里的 `dataUri` 不能以 base64 文本形式留在工具消息里——那对模型只是字符。
`AgentLoop` 把图片抽成 neutral `parts`，由 `prompt::adapt` 按供应商落线：

- OpenAI Chat：`tool` 消息不能带图片 content；工具组结束后追加一条 **user** 消息，
  content 为 `[{type:"text"},{type:"image_url",image_url:{url:"data:..."}}]`。
- Anthropic：保留在 `tool_result` 的 `{type:"image",source:{type:"base64",...}}`。
- OpenAI Responses：user 消息的 `input_image`。
- 历史回放（`project_tool_result`）丢掉 `dataUri`，只留 path/尺寸；否则每轮都重发大段 base64。
