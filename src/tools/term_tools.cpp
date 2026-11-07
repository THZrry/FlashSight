// term_tools.cpp - terminal_exec/spawn/kill/poll（回调式实现）
#include "term_tools.hpp"

#include "fs_utf8.hpp"

namespace fs {

void register_term_tools(ToolRegistry* registry) {
  // terminal_spawn（模型后台任务；前台 exec 超 5min 由宿主转后台）
  {
    ToolDef def;
    def.name = "terminal_spawn";
    def.description = "后台启动终端任务（高风险；wakeOnExit 可自动唤醒）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["command"] = Json{{"type", "string"}, {"description", "要执行的命令"}};
    props["shell"] = Json{{"type", "string"}, {"description", "shell（空=自动）"}};
    props["cwd"] = Json{{"type", "string"}, {"description", "工作目录（空=项目根）"}};
    props["wakeOnExit"] = Json{{"type", "boolean"}, {"description", "退出时自动唤醒"}};
    props["timeoutMs"] = Json{{"type", "integer"}, {"description", "超时毫秒（0=不限）"}};
    params["properties"] = props;
    params["required"] = Json::array({"command"});
    def.parameters = params;
    registry->register_tool("terminal_spawn", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.terminal_spawn) {
          return FsResult::error(ErrCode::kNotSupported, "terminal spawn not wired");
        }
        Json out;
        FsResult r = ctx.terminal_spawn(args, &out);
        if (r.ok() && !out.contains("summary")) {
          std::string command = jsonutil::get<std::string>(args, "command", "");
          out["summary"] = utf8::truncate(command, 80, "…");
        }
        return r.ok() ? FsResult::success(out) : r;
      });
  }

  // terminal_exec（前台执行，带超时；宿主可转后台）
  {
    ToolDef def;
    def.name = "terminal_exec";
    def.description = "前台执行命令（高风险；timeoutMs 超时；超 5min 宿主自动转后台）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["command"] = Json{{"type", "string"}, {"description", "要执行的命令"}};
    props["timeoutMs"] = Json{{"type", "integer"}, {"description", "超时毫秒（默认60000）"}};
    props["shell"] = Json{{"type", "string"}, {"description", "shell（空=自动）"}};
    params["properties"] = props;
    params["required"] = Json::array({"command"});
    def.parameters = params;
    registry->register_tool("terminal_exec", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.terminal_spawn) {
          return FsResult::error(ErrCode::kNotSupported, "terminal exec not wired");
        }
        Json spawn_args = args;
        spawn_args["background"] = false;
        if (!spawn_args.contains("timeoutMs")) spawn_args["timeoutMs"] = 60000;
        Json out;
        FsResult r = ctx.terminal_spawn(spawn_args, &out);
        if (!r.ok()) return r;
        // The host's foreground spawn already waits for the process and returns
        // the final output/status. Polling here duplicated the same output under
        // `poll.output` (the user saw both) and could contradict the spawn result.
        if (!out.contains("summary")) {
          std::string command = jsonutil::get<std::string>(args, "command", "");
          out["summary"] = utf8::truncate(command, 80, "…");
        }
        return FsResult::success(out);
      });
  }

  // terminal_kill
  {
    ToolDef def;
    def.name = "terminal_kill";
    def.description = "终止终端";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["tid"] = Json{{"type", "string"}, {"description", "终端 id"}};
    params["properties"] = props;
    params["required"] = Json::array({"tid"});
    def.parameters = params;
    registry->register_tool("terminal_kill", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.terminal_kill) {
          return FsResult::error(ErrCode::kNotSupported, "terminal kill not wired");
        }
        Json out;
        FsResult r = ctx.terminal_kill(args, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }

  // terminal_poll
  {
    ToolDef def;
    def.name = "terminal_poll";
    def.description = "查询终端状态/输出";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["tid"] = Json{{"type", "string"}, {"description", "终端 id"}};
    params["properties"] = props;
    params["required"] = Json::array({"tid"});
    def.parameters = params;
    registry->register_tool("terminal_poll", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        if (!ctx.terminal_poll) {
          return FsResult::error(ErrCode::kNotSupported, "terminal poll not wired");
        }
        Json out;
        FsResult r = ctx.terminal_poll(args, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }
}

}  // namespace fs