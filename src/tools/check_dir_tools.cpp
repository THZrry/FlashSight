// check_dir_tools.cpp - dir.check 子代理筛目录工具实现
// addon2.md：子代理提供需求，本工具列目录（复用 file.ls），模型按需筛查返回。
#include "check_dir_tools.hpp"

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

void register_check_dir_tools(ToolRegistry* registry) {
  ToolDef def;
  def.name = "dir.check";
  def.description =
      "列出目录并按需求筛选. List directory for subagent filtering. "
      "给子代理用/for subagents: 提供 requirement 描述要找什么, "
      "工具列出条目(名称/类型/日期/大小), 模型按需求筛查. "
      "Basic: 仅名称; detail: 名称,类型,日期,大小.";
  Json params = Json::object();
  params["type"] = "object";
  Json props = Json::object();
  props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径（默认.）"}};
  props["requirement"] = Json{{"type", "string"},
                              {"description", "筛查需求，如“找所有 .hpp 头文件”“找最近修改的测试”"}};
  props["detail"] = Json{{"type", "boolean"},
                         {"description", "详细模式（默认true，带类型/日期/大小）"}};
  props["maxResults"] = Json{{"type", "integer"}, {"description", "条目上限（默认200）"}};
  params["properties"] = props;
  params["required"] = Json::array({"requirement"});
  def.parameters = params;
  registry->register_tool("dir.check", def,
    [registry](const ToolContext& ctx, const Json& args) -> FsResult {
      std::string rel = jsonutil::get<std::string>(args, "path", ".");
      std::string requirement = jsonutil::get<std::string>(args, "requirement", "");
      bool detail = jsonutil::get<bool>(args, "detail", true);
      int maxr = static_cast<int>(jsonutil::get_int(args, "maxResults", 200));
      if (requirement.empty()) {
        return FsResult::error(ErrCode::kInvalidArg, "requirement is required");
      }
      // 复用 file.ls（注册表里已有）
      Json ls_args = Json::object();
      ls_args["path"] = rel;
      ls_args["detail"] = detail;
      FsResult r = registry->invoke(ctx, "file.ls", ls_args);
      if (!r.ok()) return r;
      Json out = r.data;
      // 基础过滤：取前 maxResults 条（详细模式 CSV 行数上限）
      if (out.contains("entries") && out["entries"].is_array()) {
        Json arr = Json::array();
        int n = 0;
        for (auto& e : out["entries"]) {
          if (n >= maxr) break;
          arr.push_back(e);
          ++n;
        }
        bool truncated = (int)out["entries"].size() > n;
        out["entries"] = arr;
        out["truncated"] = truncated;
      }
      out["requirement"] = requirement;
      out["hint"] =
          "按 requirement 筛选这些条目并只返回相关者(路径/类型/大小/修改时间)。"
          "Filter these entries per requirement, return only relevant ones.";
      return FsResult::success(out);
    });
}

}  // namespace fs