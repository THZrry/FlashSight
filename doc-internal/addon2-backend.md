# addon2 后端增强实现记录（2026-10-12）

> 依据 `rules/addon2.md` + 用户 2026-10-12 补充（notes `<15>`）。纯后端，未改 `src/ui/`。

## 阶段 A：工具补全

| 项 | 落地 | 文件 |
|---|---|---|
| A1 工具真执行 | `AgentLoop::execute_tool` + `run_loop` 循环回填；批准/拒绝后从断点续跑 | `agent_loop.cpp` |
| A2 `check_dir` | 子代理筛目录（复用 `list_dir`，带 `requirement`/`hint`） | `check_dir_tools.*` |
| A3 `file_regex` | QuickJS 正则引擎；多文件替换；`$&`/`$1..$9`/`$$`；返回 before/after 可回滚 | `regex_tools.*`、`3rdparty/qjs-regex/` |
| A4 权限门 | `resolve_by_perm`/`check_write_perm`：full 可项目外，其余仅项目内且拒绝对路径 | `file_tools.cpp` |
| A5 噪声检测 | 罕见 script 短串（阿拉伯/希伯来/泰/高棉/天城/西里尔补充）+ 引号不平衡佐证 | `noise_detect.*` |
| A6 `list_dir` 双模式 | 基础=仅文件名（默认省 token）；详细=`name,type,date,size` CSV | `file_tools.cpp` |

## 阶段 B：任务健壮性

- **B1 中断-继续**：`AgentLoop` 失败/等待批准/挂起时保存 `saved_messages_`；
  `session.continue`（`session_continue`）追加引导并 `run_loop` 续跑；发 `loop.interrupted`/`loop.resumed` 事件。
  子代理同理：`SubagentManager::steer`/`drain_steering`（子代理无轮概念，只需当前引导）。
- **B2 进程（todo）**：`TodoStore`（owner=sid 或子代理 id，独立进程树）；
  `todo_add/list/check/clear` 工具；`AgentHost::todo_op`；子代理经 spawn 时另建 ctx 得到独立树。
- **B3 计划评审**：`PlanStore::review` + `plan.reviewed`；`approve/reject/revise`，
  revise 的评价经 `steering_push(immediate)` 注入下一轮微调。

## 阶段 C：工作流

- `WorkflowStore`：定义（`workflow_define`）/实例/步骤状态机（pending/running/done/failed/skipped）；
  `graph_json()` 给侧栏图与嵌套列表（nodes=step+agent，edges=sequence+contains）。
- 运行：`workflow_start` → `start_next_pending_step` 顺序推进；
  子代理完成经 `deps.event` 的 `subagent.done` → `workflow_on_subagent_done` 推进步。
- 健壮性：步骤级 `retry_step`/`skip_step`（失败不整体中断）；`workflow_steer` 定向引导到步的子代理。

## 阶段 D：环境相关

- **D1 `lsp(lang, cmd)`**：工具 + `ctx.lsp_call` 回调（进程由宿主实现，复用 terminal 层）；
  设置键 `lsp.servers`（lang→命令）。
- **D2 浏览器**：`skill_registry` 内置 `browser-control` skill；`browser_load`（/browser-load 加载口）；
  `browser.*` 设置键已有。skill 不直接进工具列表，按需加载。

## 双语 skill

- `SkillRegistry`：中英 title/desc/body；`ThinkLang`（auto/zh/en）经设置键 `model.thinkingLang` 切换；
  auto 跟随 i18n 系统语言。工具：`load_skill` / `list_skills`。
- 全部新增工具 description 均为中英双语（简明），按 addon2 要求。

## 测试

`tests/test_addon2.cpp`：14 用例（ls 双模式、check_dir、regex 替换+逃逸、噪声检测、
权限门、todo 双层、中断-继续、计划评审、工作流图/重试/跳过/引导、双语 skill、
todo 工具层、FsApi skill/工作流/继续）。
全量：**68 用例 516 断言全过**（原 54/390 基线 + addon2 14/126）。
