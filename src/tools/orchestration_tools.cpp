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
        "更新规划树（左侧规划栏）. Update the plan tree. "
        "steps[].status: pending|running|done|skipped|blocked. "
        "未闭合的规划会让自动模式继续 / an open plan keeps auto mode going.";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"goal", obj({{"type", "string"}, {"description", "本轮总目标 overall goal"}})},
        {"steps", obj({{"type", "array"},
                       {"description", "[{title, status, note?}]"},
                       {"items", obj({{"type", "object"},
                                      {"properties", obj({
                                          {"title", obj({{"type", "string"}})},
                                          {"status", obj({{"type", "string"}})},
                                          {"note", obj({{"type", "string"}})}})}})}})},
    });
    params["required"] = Json::array({"steps"});
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
    def.description = "读取当前规划树. Read the current plan tree.";
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

  // ----- subagents ---------------------------------------------------------
  {
    ToolDef def;
    def.name = "subagent_spawn";
    def.description =
        "派子代理（上下文隔离）. Delegate to a subagent whose transcript stays out "
        "of this conversation; you get a summary back. "
        "kind: local-research|complete-step|check-files|research|explore|round-plan. "
        "For a multi-step task, spawn ONE complete-step per checklist item instead "
        "of doing the steps inline: each step's work then lives in its own "
        "transcript (visible in the side panel) and only its summary enters this "
        "conversation. "
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
    ToolDef def;
    def.name = "workflow_start";
    def.description =
        "启动一个工作流. Start a workflow run by definition name. "
        "每步是一组子代理，失败只影响该步（可重试/跳过）。";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = obj({
        {"name", obj({{"type", "string"}, {"description", "定义名 definition name"}})} });
    params["required"] = Json::array({"name"});
    def.parameters = params;
    registry->register_tool("workflow_start", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json req = args;
        req["op"] = "start";
        return need(ctx, ctx.workflow_op, req);
      });
  }
}

}  // namespace fs
