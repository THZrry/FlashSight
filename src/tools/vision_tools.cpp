// vision_tools.cpp - vision.screenshot / vision.readImage
//
// Neither is implemented yet:
//   * screenshot needs an OS screen-capture backend (Win32 GDI / X11 / Wayland)
//     and, on the desktop build, the WhaleUI window surface; there is no such
//     backend in the tree, so it reports not-supported instead of pretending.
//   * readImage needs an image decode + downscale path; the file gate already
//     exists (file_tools), but the pixel pipeline does not.
// Both are deliberately *not* faked through net_fetch (an earlier version
// forwarded a marker there, which silently issued a network request once the
// net callback was wired). They return a clear error the model can act on.
#include "vision_tools.hpp"

namespace fs {

void register_vision_tools(ToolRegistry* registry) {
  {
    ToolDef def;
    def.name = "vision.screenshot";
    def.description = "截图（screen/window/region）。尚未接线：需要平台截图后端。";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["target"] = Json{{"type", "string"}, {"description", "screen/window/region"}};
    props["region"] = Json{{"type", "string"}, {"description", "区域 x,y,w,h（target=region 时）"}};
    params["properties"] = props;
    def.parameters = params;
    registry->register_tool("vision.screenshot", def,
      [](const ToolContext&, const Json&) -> FsResult {
        return FsResult::error(
            ErrCode::kNotSupported,
            "vision.screenshot not wired: needs a platform capture backend");
      });
  }

  {
    ToolDef def;
    def.name = "vision.readImage";
    def.description = "读取本地图片（走文件门；maxSide/quality 压缩）。尚未接线：需要图像解码/缩放后端。";
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
      [](const ToolContext&, const Json&) -> FsResult {
        return FsResult::error(
            ErrCode::kNotSupported,
            "vision.readImage not wired: needs an image decode/resize backend");
      });
  }
}

}  // namespace fs
