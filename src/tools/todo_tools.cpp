// todo_tools.cpp - 进程（todo）工具实现（addon2.md 长任务-进程）
// 工具层只转发到宿主 todo_op 回调（状态由 AgentHost 的 TodoStore 持有）。
#include "todo_tools.hpp"

namespace fs {

namespace {

// 统一的转发：args 原样给宿主，宿主返回 {items:[...]} / {id:n}
FsResult forward(const ToolContext& ctx, const Json& args) {
  if (!ctx.todo_op) {
    return FsResult::error(ErrCode::kNotSupported, "todo not wired");
  }
  Json out;
  FsResult r = ctx.todo_op(args, &out);
  return r.ok() ? FsResult::success(out) : r;
}

Json base_params() {
  Json params = Json::object();
  params["type"] = "object";
  return params;
}

}  // namespace

void register_todo_tools(ToolRegistry* registry) {
  // todo_add
  {
    ToolDef def;
    def.name = "todo_add";
    def.description =
        "新增进程任务. Add a task to the current process list. "
        "列出本轮任务，完成一项用 todo_check 打勾.";
    Json params = base_params();
    Json props = Json::object();
    props["task"] = Json{{"type", "string"}, {"description", "任务描述"}};
    params["properties"] = props;
    params["required"] = Json::array({"task"});
    def.parameters = params;
    registry->register_tool("todo_add", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json a = args;
        a["op"] = "add";
        return forward(ctx, a);
      });
  }

  // todo_list
  {
    ToolDef def;
    def.name = "todo_list";
    def.description =
        "查看进程任务列表. List the current process tasks (with done flags).";
    Json params = base_params();
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("todo_list", def,
      [](const ToolContext& ctx, const Json&) -> FsResult {
        return forward(ctx, Json{{"op", "list"}});
      });
  }

  // todo_check / todo.uncheck
  {
    ToolDef def;
    def.name = "todo_check";
    def.description =
        "勾选/取消勾选进程任务. Mark a process task done (done=true) or not (done=false).";
    Json params = base_params();
    Json props = Json::object();
    props["id"] = Json{{"type", "integer"}, {"description", "任务 id"}};
    props["done"] = Json{{"type", "boolean"}, {"description", "true=打勾，false=取消（默认true）"}};
    params["properties"] = props;
    params["required"] = Json::array({"id"});
    def.parameters = params;
    registry->register_tool("todo_check", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json a = args;
        a["op"] = "check";
        return forward(ctx, a);
      });
  }

  // todo_clear
  {
    ToolDef def;
    def.name = "todo_clear";
    def.description = "清空进程任务. Clear the current process task list.";
    Json params = base_params();
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("todo_clear", def,
      [](const ToolContext& ctx, const Json&) -> FsResult {
        return forward(ctx, Json{{"op", "clear"}});
      });
  }
}

}  // namespace fs