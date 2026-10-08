# UI 事件与页面（ui-events）

> 决策来源：goal §界面、addons §9；QA Q8、Q19–Q22。
> 前提：UI 按**不支持 JS 的 webview**规划，DOM 操作是全的；
> WhaleUI API 未完善，本文件只定“画什么、推什么”，不定控件 API。

## 1. 页面 IA

- 主页：探索（v1 灰色禁用）/ 项目（历史项目 + 新建：新建文件夹/打开文件夹/远程连接入口）/
  用量统计 / 设置 / 新建。
- 会话页（上下 + 双侧栏）：
  - 顶部栏：tab / 模型 / 思考强度 / 权限 / 自动挡 / 挂起（待唤醒与中断共用按钮位、不同图标）。
  - 左悬浮规划栏：plan 树 + git 摘要（branch/提交态/更新时间 → 跳侧栏）。
  - 中间历史：虚拟化一次 10 条；历史渲染一次、当前实时渲染；每条可复制/分叉/回滚/
    遗忘/看 diff；已完成 chat 默认折叠只留 summary（可展开）；输入框区域显示待唤醒信息。
  - 右标签侧栏：用量&上下文、本地文件、文件操作、git 历史分支、子代理历史、
    toolcall 详情、历史输入详情；可折叠。
  - 底部输入框：/ 指令与文件补全上浮；markdown 高亮不渲染；默认 Enter 发送、
    Shift+Enter 换行（可切 Ctrl+Enter）；截图/图片附件入口。
  - 终端区：默认隐藏；集合页 + 编号页；手动停止；显示当前 shell。
- 新建标签页：打开已有 / 在项目中新建会话 / 新建项目（复用主页项目逻辑）。
- 设置页：经典双栏（通用/模型/权限/其他四组，见 provider.md §5 与 goal）。
  探索/插件入口保留但灰色不可点；v1 无 `plugin.*`。
- 无边框自定义标题栏（Win/Linux X11，最大/最小/关闭 + 侧栏管理钮）；
  默认横向 tab，可切垂直 tab；悬浮 tooltip 显示全名。

## 2. 后端→UI 事件表（v1）

| 事件 | 载荷要点 | UI 行为 |
|---|---|---|
| `stream.delta` | sid, turnId, text | 当前对话实时渲染（diff 更新） |
| `turn.done` | sid, turnId, summary | 历史追加一条，折叠态 |
| `tool.update` | name, args?, 进展 | toolcall 详情 + 文件操作视图更新 |
| `permission.prompt` | 动作, 目标 | 批准弹窗（approve 档） |
| `ask.prompt` | question, options, recommended | 手动模式弹窗等用户选 |
| `plan.update` | planTree delta | 规划栏更新 |
| `git.update` | branch, dirty, time | 规划栏 git 摘要更新 |
| `terminal.output/exit` | tid, chunk / exitCode | 终端页更新；后台退出→徽标或唤醒 |
| `lock.change` | pid, holder | 写锁提示更新 |
| `wake.fired` | sid, source | 待唤醒徽标/输入框区信息 |
| `usage.update` | 调用用量 | 用量小视图 + 用量页更新 |
| `config.refreshed` | configVersion | 提示条“配置已刷新” |
| `truncate.hint` | turnId, 位置 | 输出块下加截断/折叠提示条（不自动截） |
| `subagent.done` | summaryRef | 子代理历史更新；自动模式唤醒父 |

## 3. i18n / 主题 / 输入

- 语言默认跟随系统（中英先行），语言文件编译时预处理进 cpp，预留外部翻译渠道。
- 主题多套配色、每套深浅色（WhaleUI `@media` 查询）；字体设置、代码高亮在设置-通用-外观。
- 快捷键 v1 只做基础设施（可配表），默认键位以后再排。
- openWith（文件管理器/vscode/vs/sublime/kate/jetbrains/系统默认）是右键下拉组合按钮，
  纯 GUI 功能，路径模板在设置-环境管理。

## 4. 性能与空态（验收条目）

- 历史虚拟化（一次 10 条）、diff div 限高滚动超长截断、流式渲染 diff 更新（md4c diff AST）。
- 空项目/空会话/无网络/长列表/超大 diff 均要有空态与错误态，不白屏。
