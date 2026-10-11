// vision_tools.cpp - vision.screenshot/readImage（回调式）
#include "vision_tools.hpp"

namespace fs {

void register_vision_tools(ToolRegistry* registry) {
  // vision.screenshot（屏/窗口/区域走权限门；截图数据由宿主回调提供）
  {
    ToolDef def;
    def.name = "vision.screenshot";
    def.description = "截图（screen/window/region；返回图像引用）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["target"] = Json{{"type", "string"}, {"description", "screen/window/region"}};
    props["region"] = Json{{"type", "string"}, {"description", "区域 x,y,w,h（target=region 时）"}};
    params["properties"] = props;
    def.parameters = params;
    registry->register_tool("vision.screenshot", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        // 截图由宿主回调（通过 emit 请求宿主截图，再由宿主回填）
        // v1：通过 net_fetch 的特殊标记转发（宿主识别 screenshot 标记）
        if (!ctx.net_fetch) {
          return FsResult::error(ErrCode::kNotSupported, "screenshot not wired");
        }
        Json fwd = Json::object();
        fwd["screenshot"] = true;
        fwd["target"] = jsonutil::get<std::string>(args, "target", "screen");
        fwd["region"] = jsonutil::get<std::string>(args, "region", "");
        Json out;
        FsResult r = ctx.net_fetch(fwd, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }

  // vision.readImage（读本地图；走文件门；最大边+质量压缩可设）
  {
    ToolDef def;
    def.name = "vision.readImage";
    def.description = "读取本地图片（走文件门；maxSide/quality 可设压缩）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内图片路径"}};
    props["maxSide"] = Json{{"type", "integer"}, {"description", "最大边像素（默认1024）"}};
    props["quality"] = Json{{"type", "integer"}, {"description", "质量 1-100（默认80）"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("vision.readImage", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        // 读图走文件门（宿主实现；通过 net_fetch 特殊标记转发）
        if (!ctx.net_fetch) {
          return FsResult::error(ErrCode::kNotSupported, "readImage not wired");
        }
        Json fwd = Json::object();
        fwd["readImage"] = true;
        fwd["path"] = jsonutil::get<std::string>(args, "path", "");
        fwd["maxSide"] = jsonutil::get_int(args, "maxSide", 1024);
        fwd["quality"] = jsonutil::get_int(args, "quality", 80);
        Json out;
        FsResult r = ctx.net_fetch(fwd, &out);
        return r.ok() ? FsResult::success(out) : r;
      });
  }
}

}  // namespace fs