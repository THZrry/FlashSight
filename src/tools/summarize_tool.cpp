// summarize_tool.cpp - session.summarize 实现
// 要点经 ToolContext.emit 以 session.keypoint 事件发给宿主，由宿主写入当前 turn。
#include "summarize_tool.hpp"

namespace fs {

void register_summarize_tool(ToolRegistry* registry) {
  ToolDef def;
  def.name = "session.summarize";
  def.description = "记录本轮要点（持久记忆；压缩时自动附加到上下文）";
  Json params = Json::object();
  params["type"] = "object";
  Json props = Json::object();
  props["points"] = Json{{"type", "array"}, {"description", "要点列表（字符串数组）"}};
  params["properties"] = props;
  params["required"] = Json::array({"points"});
  def.parameters = params;
  registry->register_tool("session.summarize", def,
    [](const ToolContext& ctx, const Json& args) -> FsResult {
      Json points = args.contains("points") && args["points"].is_array()
                        ? args["points"] : Json::array();
      if (points.empty()) {
        return FsResult::error(ErrCode::kInvalidArg, "empty points");
      }
      // 发事件给宿主落库（宿主写入当前 turn.keyPoints）
      if (ctx.emit) {
        Json payload = Json::object();
        payload["points"] = points;
        ctx.emit("session.keypoint", payload);
      }
      Json data = Json::object();
      data["recorded"] = (int)points.size();
      return FsResult::success(data);
    });
}

}  // namespace fs