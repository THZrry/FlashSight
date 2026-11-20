// orchestration_tools.cpp - plan / subagent / workflow tools
#include "orchestration_tools.hpp"

namespace fs {

namespace {

FsResult need(const ToolContext& ctx,
              const std::function<FsResult(const Json&, Json*)>& fn,
              const Json& args) {
  if (!fn) return FsResult::error(ErrCode::kNotSupported, "host did not wire this tool");
  Json out = Json::object();
  FsResult r = fn(args, &out);
  if (!r.ok()) return r;
  return FsResult::success(out);
}

Json obj(std::initializer_list<std::pair<const char*, Json>> init) {
  Json j = Json::object();
  for (auto& kv : init) j[kv.first] = kv.second;
  return j;
}

}  // namespace

void register_orchestration_tools(ToolRegistry* registry) {
  // ----- plan --------------------------------------------------------------
  {
    ToolDef def;
    def.name = "plan_update";
    def.description =
        "写规划文档（会话目录 plans/ 下的编号 markdown）。The plan is a markdown "
        "document you own: write the whole task plan into it once (goal, approach, "
        "acceptance), then each round derive the next 任务列表 (todo_add) from it. "
        "Update the document when the approach changes. Start a NEW task plan with "
        "new=true (numbered, old plans stay). When EVERYTHING in it is "
        "done, call this with status=\"done\" - an active document is what keeps "
        "auto mode going. content replaces the document; omit it to only change "
        "the status.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"content", obj({{"type", "string"},
                         {"description",
                          "整篇规划文档（markdown，全量替换）. The full document; "
                          "omit to keep the current body."}})},
        {"status", obj({{"type", "string"},
                        {"enum", Json::array({"active", "done"})},
                        {"description",
                         "active=还在做（默认，会让自动模式继续）；"
                         "done=规划完成（自动模式停止）. active is the default."}})},
        {"new", obj({{"type", "boolean"},
                     {"description",
                      "true=开始下一篇规划（旧文档保留，编号+1）；默认 false. "
                      "Use true when starting a NEW task plan instead of rewriting "
                      "the current one."}})},
    });
    def.parameters = params;
    registry->register_tool("plan_update", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "update";
        return need(ctx, ctx.plan_op, req);
      });
  }
  {
    ToolDef def;
    def.name = "plan_get";
    def.description =
        "读取当前规划文档（会话目录 plans/ 下编号最大的一篇）. Read the current plan "
        "document: "
        "{exists, status, content, path}. Re-read it before deciding the next "
        "task list.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("plan_get", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = Json::object();
        req["op"] = "get";
        return need(ctx, ctx.plan_op, req);
      });
  }

  // ----- scheduled wake ----------------------------------------------------
  {
    ToolDef def;
    def.name = "wake_schedule";
    def.description =
        "安排一次定时唤醒：delayMs 之后自动重新开始本会话的一轮。Schedule ONE "
        "future turn. Do NOT sleep inside terminal_exec for a delay: that pins a "
        "turn and a text-only wait produces nothing. Call this, end the current "
        "turn, and the scheduler will resume you with a SYSTEM note at the due "
        "time; put what should happen then in `note` (it is shown back to you). "
        "Only one pending wake per session; scheduling again replaces it.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"delayMs", obj({{"type", "integer"},
                         {"description", "从现在起多少毫秒后唤醒 (>=1)"}})},
        {"note", obj({{"type", "string"},
                      {"description", "到点后要做什么（自包含，会作为唤醒提示回传）"}})},
    });
    params["required"] = Json::array({"delayMs"});
    def.parameters = params;
    registry->register_tool("wake_schedule", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "schedule";
        return need(ctx, ctx.wake_op, req);
      });
  }
  {
    ToolDef def;
    def.name = "wake_cancel";
    def.description = "取消本会话尚未到点的唤醒. Cancel the pending scheduled wake.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("wake_cancel", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)args;
        return need(ctx, ctx.wake_op, Json{{"op", "cancel"}});
      });
  }
  {
    ToolDef def;
    def.name = "wake_list";
    def.description = "查看本会话的待唤醒事件. List pending wakes for this session.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("wake_list", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        (void)args;
        return need(ctx, ctx.wake_op, Json{{"op", "list"}});
      });
  }

  // ----- subagents ---------------------------------------------------------
  {
    ToolDef def;
    def.name = "subagent_spawn";
    def.description =
        "派子代理（上下文隔离）. Delegate to a subagent whose transcript stays out "
        "of this conversation; you get a summary back. "
        "kind: local-research|complete-step|check-files|research|explore|round-plan. "
        "complete-step ALWAYS blocks and returns its summary; the work lives in "
        "the subagent transcript (shown inline while it runs, then kept in the "
        "side panel) and only the summary enters this conversation. "
        "After planning a multi-step job, you MUST use one complete-step per "
        "checklist item instead of doing the steps inline: this is how the main "
        "context stays small. "
        "task 必须自包含（子代理看不到本会话）. The task must be self-contained.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"kind", obj({{"type", "string"},
                      {"description", "预置代理名 preset agent name"}})},
        {"task", obj({{"type", "string"},
                      {"description", "目标 + 已知信息 + 验收标准 goal + facts + acceptance"}})},
        {"step", obj({{"type", "string"},
                      {"description", "清单项标题（用于进度显示与结果归档）checklist item"}})},
        {"todoId", obj({{"type", "integer"},
                        {"description", "对应 todo 项 id；完成后自动勾选"}})},
        {"model", obj({{"type", "string"}, {"description", "空=继承父空 inherit"}})},
        {"timeoutMs", obj({{"type", "integer"}, {"description", "0=不限"}})},
        {"wait", obj({{"type", "boolean"},
                      {"description", "true=阻塞等结果 blocking (default false)"}})},
    });
    params["required"] = Json::array({"kind", "task"});
    def.parameters = params;
    registry->register_tool("subagent_spawn", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "spawn";
        return need(ctx, ctx.subagent_op, req);
      });
  }
  {
    // A first-class name for the most important delegation. The model had to
    // spell `subagent_spawn(kind:"complete-step", wait:true)` and mostly did not
    // use it, doing the steps inline; a dedicated tool with a short description
    // is what makes it the default for implementation work.
    ToolDef def;
    def.name = "complete_step";
    def.description =
        "执行一个独立实现步骤（上下文隔离，阻塞返回摘要）. Run ONE self-contained "
        "implementation step in an isolated subagent, blocking until it reports, "
        "and get only its summary back into this conversation. The real tool "
        "calls are shown inline while it works and then archived in the subagent "
        "panel. USE THIS FOR EVERY implementation step: any work that reads or "
        "changes multiple files or runs several commands is one step, and doing "
        "it inline is what makes the main context explode. Do not wait for the "
        "user to ask; after planning, delegate step by step. `task` must be "
        "self-contained (the subagent cannot see this conversation).";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"task", obj({{"type", "string"},
                      {"description", "目标 + 已知信息 + 验收标准（自包含）"}})},
        {"step", obj({{"type", "string"},
                      {"description", "清单项标题（用于进度显示与结果归档）"}})},
        {"todoId", obj({{"type", "integer"},
                        {"description", "对应 todo 项 id；完成后自动勾选"}})},
        {"model", obj({{"type", "string"}, {"description", "空=继承父"}})},
        {"timeoutMs", obj({{"type", "integer"}, {"description", "0=不限"}})},
    });
    params["required"] = Json::array({"task"});
    def.parameters = params;
    registry->register_tool("complete_step", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "spawn";
        req["kind"] = "complete-step";
        req["wait"] = true;
        return need(ctx, ctx.subagent_op, req);
      });
  }
  {
    ToolDef def;
    def.name = "subagent_list";
    def.description = "列出本会话的子代理及状态. List this session's subagents.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("subagent_list", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = Json::object();
        req["op"] = "list";
        return need(ctx, ctx.subagent_op, req);
      });
  }
  {
    ToolDef def;
    def.name = "subagent_follow_up";
    def.description =
        "给已结束的子代理追加任务，让它在原有上下文里继续干（同步返回新的摘要）. "
        "Continue a FINISHED subagent with more work in the same context, "
        "returning the new summary. Use this instead of spawning a second agent "
        "for the same area: the run keeps the earlier transcript, so the subagent "
        "still knows what it already read and changed.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"id", obj({{"type", "string"}, {"description", "子代理 id（subagent_spawn 返回的）"}})},
        {"text", obj({{"type", "string"},
                      {"description", "追加的任务/引导（自包含：要做什么、验收标准）"}})}});
    params["required"] = Json::array({"id", "text"});
    def.parameters = params;
    registry->register_tool("subagent_follow_up", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "followUp";
        return need(ctx, ctx.subagent_op, req);
      });
  }
  {
    ToolDef def;
    def.name = "subagent_cancel";
    def.description = "取消子代理. Cancel a running subagent.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"id", obj({{"type", "string"}, {"description", "子代理 id"}})} });
    params["required"] = Json::array({"id"});
    def.parameters = params;
    registry->register_tool("subagent_cancel", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "cancel";
        return need(ctx, ctx.subagent_op, req);
      });
  }
  {
    ToolDef def;
    def.name = "subagent_steer";
    def.description =
        "给运行中的子代理追加引导. Send additional guidance to a running subagent.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"id", obj({{"type", "string"}, {"description", "子代理 id"}})},
        {"text", obj({{"type", "string"}, {"description", "引导内容 guidance"}})}});
    params["required"] = Json::array({"id", "text"});
    def.parameters = params;
    registry->register_tool("subagent_steer", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "steer";
        return need(ctx, ctx.subagent_op, req);
      });
  }

  // ----- workflow ----------------------------------------------------------
  {
    ToolDef def;
    def.name = "workflow_list";
    def.description =
        "列出可用工作流定义与运行实例. List workflow definitions and runs. "
        "只有需要反复执行/逐步重试的流水线才用工作流，一次性任务用 todo。";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("workflow_list", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = Json::object();
        req["op"] = "list";
        return need(ctx, ctx.workflow_op, req);
      });
  }
  {
    // 规划工作流：模型提出步骤，用户允许后才执行。
    //
    // This replaced `workflow_start`, which let the MODEL start a pipeline on its
    // own. That is not what a workflow is here: the user approves the plan first
    // (the sidebar shows it with 允许 / 拒绝), because a pipeline spends real time
    // and tokens across several subagents.
    ToolDef def;
    def.name = "workflow_plan";
    def.description =
        "规划一个工作流并提交给用户批准. Propose a multi-step workflow for the "
        "USER TO APPROVE; nothing runs until they allow it. 每步由子代理执行，"
        "失败只影响该步（可重试/跳过）。一次性任务用 todo，不要用这个。";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"goal", obj({{"type", "string"},
                      {"description", "总体目标（工作流名）goal / workflow name"}})},
        {"steps", obj({{"type", "array"},
                       {"description",
                        "[{title, task?, kind?}]：title 是一步的名字，task 给子代理的"
                        "具体任务（默认等于 title），kind 默认 complete-step"}})} });
    params["required"] = Json::array({"goal", "steps"});
    def.parameters = params;
    registry->register_tool("workflow_plan", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "propose";
        return need(ctx, ctx.workflow_op, req);
      });
  }
  {
    // Emergency brake for an automatic run. This is deliberately a TOOL the
    // model must call explicitly, not a normal control: it exists for the case
    // where continuing would be wrong or impossible (the task cannot proceed,
    // the plan is contradictory, the user must decide something). Ordinary
    // uncertainty should be parked in the plan/todo and other work continued.
    ToolDef def;
    def.name = "auto_stop";
    def.description =
        "强制结束自动续跑（仅当确实无法继续或必须由用户决定时使用，平常不要用）。"
        "Emergency stop for AUTO mode. Do NOT use it for ordinary uncertainty: put "
        "the blocked item in the plan/todo and continue other work. Use it only "
        "when no safe next step exists; the current turn finishes and no further "
        "auto round starts.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"reason", obj({{"type", "string"},
                        {"description", "为什么必须停止 / why no safe step remains"}})},
    });
    params["required"] = Json::array({"reason"});
    def.parameters = params;
    registry->register_tool("auto_stop", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.stop_auto) {
          return FsResult::error(ErrCode::kNotSupported, "auto mode not wired");
        }
        FsResult r = ctx.stop_auto();
        if (!r.ok()) return r;
        return FsResult::success(
            Json{{"stopped", true},
                 {"reason", jsonutil::get<std::string>(args, "reason", "")}});
      });
  }
}

}  // namespace fs
