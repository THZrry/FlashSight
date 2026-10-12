// skill_registry.cpp - 中英双语 skill 注册表实现（addon2.md）
#include "skill_registry.hpp"

#include "common/fs_i18n.hpp"

namespace fs {

const char* think_lang_str(ThinkLang l) {
  switch (l) {
    case ThinkLang::kZh: return "zh";
    case ThinkLang::kEn: return "en";
    case ThinkLang::kAuto: return "auto";
  }
  return "auto";
}

ThinkLang parse_think_lang(const std::string& s) {
  if (s == "zh" || s == "zh-CN" || s == "cn") return ThinkLang::kZh;
  if (s == "en" || s == "en-US") return ThinkLang::kEn;
  return ThinkLang::kAuto;
}

ThinkLang effective_think_lang(ThinkLang l) {
  if (l != ThinkLang::kAuto) return l;
  // auto → 跟随 i18n 系统语言
  return i18n::effective_lang() == i18n::Lang::kEn ? ThinkLang::kEn : ThinkLang::kZh;
}

std::vector<Skill> builtin_skills() {
  std::vector<Skill> v;

  // 浏览器控制（addon2：做成 skill，按需加载，不直接提供）
  {
    Skill s;
    s.name = "browser-control";
    s.title_zh = "浏览器控制";
    s.title_en = "Browser Control";
    s.desc_zh = "控制外部浏览器（选择/查询元素、提交事件、执行 JS）";
    s.desc_en = "Drive an external browser (select/query elements, dispatch events, run JS)";
    s.body_zh =
        "用法：先确认设置-浏览器控制已开启并配置 Browser Driver。\n"
        "可用操作：\n"
        "- 打开 URL / 导航 / 后退\n"
        "- 选择元素（CSS 选择器）/ 查询属性与文本\n"
        "- 提交事件（click/input/submit）\n"
        "- 执行 JS 片段并取回结果\n"
        "建议流程：navigate -> query(定位) -> act(操作) -> 截图/校验。\n"
        "注意：wasm 模式下在侧栏启用。";
    s.body_en =
        "Usage: first ensure Settings > Browser Control is enabled with a Browser Driver.\n"
        "Operations:\n"
        "- open URL / navigate / back\n"
        "- select elements (CSS selector) / query attributes and text\n"
        "- dispatch events (click/input/submit)\n"
        "- run a JS snippet and read back the result\n"
        "Suggested flow: navigate -> query -> act -> screenshot/verify.\n"
        "Note: in wasm mode enable it from the sidebar.";
    s.loadable = true;
    v.push_back(s);
  }

  // LSP（addon2：内置 MCP，适合符号重构/整项目级操作）
  {
    Skill s;
    s.name = "lsp";
    s.title_zh = "LSP 语言服务";
    s.title_en = "LSP Language Server";
    s.desc_zh = "经 LSP 服务器做符号重构与整项目级操作";
    s.desc_en = "Symbol refactor and project-wide operations via LSP servers";
    s.body_zh =
        "用法：在设置-环境中配置各语言 LSP 服务器（可自动在 PATH 搜索）。\n"
        "调用：lsp(lang, cmd) 确保对应服务器启动并执行命令。\n"
        "适用：重命名符号、查找引用、整项目级修改。\n"
        "先提供语言列表，模型按需选择。";
    s.body_en =
        "Usage: configure LSP servers per language in Settings > Environment (auto-search PATH).\n"
        "Call: lsp(lang, cmd) ensures the server is started and runs the command.\n"
        "Good for: rename symbol, find references, project-wide edits.\n"
        "Provide the language list first; the model picks on demand.";
    s.loadable = true;
    v.push_back(s);
  }

  return v;
}

SkillRegistry::SkillRegistry() : skills_(builtin_skills()) {}

bool SkillRegistry::has(const std::string& name) const {
  for (auto& s : skills_) {
    if (s.name == name) return true;
  }
  return false;
}

FsResult SkillRegistry::load(const std::string& name, ThinkLang lang) {
  ThinkLang eff = effective_think_lang(lang);
  for (auto& s : skills_) {
    if (s.name != name) continue;
    Json data = Json::object();
    data["name"] = s.name;
    bool zh = (eff == ThinkLang::kZh);
    data["title"] = zh ? s.title_zh : s.title_en;
    data["lang"] = think_lang_str(eff);
    data["body"] = zh ? s.body_zh : s.body_en;
    return FsResult::success(data);
  }
  return FsResult::error(ErrCode::kNotFound, "skill not found: " + name);
}

FsResult SkillRegistry::brief(const std::string& name, ThinkLang lang) {
  ThinkLang eff = effective_think_lang(lang);
  bool zh = (eff == ThinkLang::kZh);
  for (auto& s : skills_) {
    if (s.name != name) continue;
    Json data = Json::object();
    data["name"] = s.name;
    data["title"] = zh ? s.title_zh : s.title_en;
    data["description"] = zh ? s.desc_zh : s.desc_en;
    return FsResult::success(data);
  }
  return FsResult::error(ErrCode::kNotFound, "skill not found: " + name);
}

Json SkillRegistry::list_json(ThinkLang lang) const {
  ThinkLang eff = effective_think_lang(lang);
  bool zh = (eff == ThinkLang::kZh);
  Json arr = Json::array();
  for (auto& s : skills_) {
    Json j = Json::object();
    j["name"] = s.name;
    j["title"] = zh ? s.title_zh : s.title_en;
    j["description"] = zh ? s.desc_zh : s.desc_en;
    j["loadable"] = s.loadable;
    arr.push_back(j);
  }
  return arr;
}

void register_skill_tools(ToolRegistry* registry, SkillRegistry* skills) {
  // tool.skill.load：按需加载 skill 正文（addon2：不直接提供，按需加载）
  {
    ToolDef def;
    def.name = "tool.skill.load";
    def.description =
        "加载 skill 详细用法. Load a skill's detailed usage on demand. "
        "如 browser-control / lsp. 按设置思考语言返回中/英.";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["name"] = Json{{"type", "string"}, {"description", "skill 名称"}};
    params["properties"] = props;
    params["required"] = Json::array({"name"});
    def.parameters = params;
    registry->register_tool("tool.skill.load", def,
      [skills](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string name = jsonutil::get<std::string>(args, "name", "");
        if (name.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty skill name");
        return skills->load(name, parse_think_lang(ctx.thinking_lang));
      });
  }

  // tool.skill.list：列出可用 skill 简述
  {
    ToolDef def;
    def.name = "tool.skill.list";
    def.description = "列出可用 skill. List available skills (bilingual).";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("tool.skill.list", def,
      [skills](const ToolContext& ctx, const Json&) -> FsResult {
        return FsResult::success(skills->list_json(parse_think_lang(ctx.thinking_lang)));
      });
  }
}

}  // namespace fs