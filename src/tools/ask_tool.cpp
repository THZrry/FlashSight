// ask_tool.cpp - ask 工具实现
//
// The tool does NOT answer itself. It reports "waiting for the user" and the
// LOOP parks the turn on it, exactly like the permission gate. The first version
// emitted `ask.prompt` and returned success, so the model was told "asked:true"
// as if the user had already replied: the question appeared in the UI but the
// answer never reached the model, the turn carried on regardless, and the model
// often asked again (which is also what drove turns into the tool-round cap).
#include "ask_tool.hpp"

namespace fs {

void register_ask_tool(ToolRegistry* registry) {
  ToolDef def;
  def.name = "ask";
  def.description =
      "向用户提问并等待回答（手动模式；自动模式不可用）. Ask the user and WAIT. "
      "The turn pauses until they choose; their choice is returned to you as this "
      "tool's result. Provide question + options, and mark the option you "
      "recommend with `recommended`.";
  Json params = Json::object();
  params["type"] = "object";
  Json props = Json::object();
  props["question"] = Json{{"type", "string"}, {"description", "问题"}};
  props["options"] = Json{{"type", "array"}, {"description", "选项列表"}};
  props["recommended"] = Json{{"type", "integer"}, {"description", "推荐选项下标"}};
  props["allowCustom"] = Json{{"type", "boolean"}, {"description", "允许自定义输入"}};
  params["properties"] = props;
  params["required"] = Json::array({"question", "options"});
  def.parameters = params;
  registry->register_tool("ask", def,
    [](const ToolContext& ctx, const Json& args) -> FsResult {
      std::string question = jsonutil::get<std::string>(args, "question", "");
      if (question.empty()) {
        return FsResult::error(ErrCode::kInvalidArg, "empty question");
      }
      if (ctx.auto_mode) {
        // 自动模式：ask 已从工具列表移除；若模型硬调则拒绝
        return FsResult::error(ErrCode::kBusy, "ask not available in auto mode");
      }
      Json data = Json::object();
      // `awaiting_user` is the signal the loop parks on. The real result is
      // filled in by AgentLoop::answer_ask when the user picks an option.
      data["awaiting_user"] = true;
      data["question"] = question;
      return FsResult::success(data);
    });
}

}  // namespace fs
