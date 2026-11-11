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
        "新增进程任务（一次可加多条）. Add one or more tasks to the current "
        "process list; pass `tasks` for a batch. Returns only ids/pending counts, "
        "not the whole list (use todo_list when you need the text).";
    Json params = base_params();
    Json props = Json::object();
    props["task"] = Json{{"type", "string"}, {"description", "单个任务描述"}};
    props["tasks"] = Json{
        {"type", "array"},
        {"items", Json{{"type", "string"}}},
        {"description", "一次添加多条任务（与 task 二选一或同时给）"}};
    params["properties"] = props;
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

  // todo_erase
  {
    ToolDef def;
    def.name = "todo_erase";
    def.description =
        "删除一条进程任务（不做完也不要了）. Remove ONE task from the current "
        "process list by id. Use it to drop an obsolete/duplicate item instead of "
        "marking it done or clearing the whole list.";
    Json params = base_params();
    Json props = Json::object();
    props["id"] = Json{{"type", "integer"}, {"description", "任务 id"}};
    params["properties"] = props;
    params["required"] = Json::array({"id"});
    def.parameters = params;
    registry->register_tool("todo_erase", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        Json a = args;
        a["op"] = "erase";
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