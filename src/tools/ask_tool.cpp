// ask_tool.cpp - ask 工具实现
#include "ask_tool.hpp"

namespace fs {

void register_ask_tool(ToolRegistry* registry) {
  ToolDef def;
  def.name = "ask";
  def.description = "向用户提问（手动模式弹窗等选；自动模式不可用）";
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
      // 发 ask.prompt 事件（UI 弹窗；选择由 Loop.answer_ask 回填）
      if (ctx.emit) {
        Json payload = Json::object();
        payload["question"] = question;
        payload["options"] = args.contains("options") ? args["options"] : Json::array();
        payload["recommended"] = jsonutil::get_int(args, "recommended", 0);
        payload["allowCustom"] = jsonutil::get<bool>(args, "allowCustom", true);
        ctx.emit("ask.prompt", payload);
      }
      Json data = Json::object();
      data["asked"] = true;
      data["question"] = question;
      return FsResult::success(data);
    });
}

}  // namespace fs