// skill_registry.cpp - 中英双语 skill 注册表实现（addon2.md）
#include "skill_registry.hpp"

#include <fstream>
#include <sstream>

#include <ghc/filesystem.hpp>

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

  // 浏览器控制 has NO skill entry on purpose.
  //
  // It used to describe navigate/query/act/eval and point at a
  // "Settings > Browser Control" page. No driver and no tool ever existed
  // behind it: loading it told the model to call operations that are not
  // registered, and the settings page it referenced was itself a wall of inert
  // switches (removed with it). A capability that only exists as documentation
  // is worse than a missing one - it turns every UI-verification task into the
  // model inventing results. Verification drives a REAL browser (Playwright /
  // the local engine) through the terminal tool instead.

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
    s.kind = "mcp";  // 内置 MCP
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
    j["kind"] = s.kind;
    if (!s.dir.empty()) j["dir"] = s.dir;
    j["user"] = s.user;
    j["enabled"] = s.enabled;
    arr.push_back(j);
  }
  return arr;
}

// ----- 用户 skill -----
// 一个 skill 目录里放一份 SKILL.md（正文）+ 可选 frontmatter 里的 name/title。
// 也接受直接放一个 <name>.md。这样用户把一个 skill 文件夹丢进来就能用，不用学
// 一套额外的清单格式。
namespace {

// 取 frontmatter 里 `key: value` 的值（没有 frontmatter 就返回空）
std::string front_value(const std::string& text, const std::string& key) {
  if (text.compare(0, 3, "---") != 0) return std::string();
  size_t end = text.find("\n---", 3);
  if (end == std::string::npos) return std::string();
  std::string head = text.substr(3, end - 3);
  size_t i = 0;
  while (i < head.size()) {
    size_t e = head.find('\n', i);
    if (e == std::string::npos) e = head.size();
    std::string line = head.substr(i, e - i);
    size_t c = line.find(':');
    if (c != std::string::npos) {
      std::string k = line.substr(0, c);
      std::string v = line.substr(c + 1);
      // trim both sides
      size_t a = 0, b = k.size();
      while (a < b && (k[a] == ' ' || k[a] == '\t')) ++a;
      while (b > a && (k[b - 1] == ' ' || k[b - 1] == '\t')) --b;
      k = k.substr(a, b - a);
      a = 0; b = v.size();
      while (a < b && (v[a] == ' ' || v[a] == '\t')) ++a;
      while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\r')) --b;
      v = v.substr(a, b - a);
      if (k == key) return v;
    }
    if (e == head.size()) break;
    i = e + 1;
  }
  return std::string();
}

std::string strip_frontmatter(const std::string& text) {
  if (text.compare(0, 3, "---") != 0) return text;
  size_t end = text.find("\n---", 3);
  if (end == std::string::npos) return text;
  size_t nl = text.find('\n', end + 1);
  return nl == std::string::npos ? std::string() : text.substr(nl + 1);
}

std::string read_text(const ghc::filesystem::path& p) {
  std::ifstream ifs(p.string(), std::ios::binary);
  if (!ifs) return std::string();
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

}  // namespace

int SkillRegistry::import_dir(const std::string& dir) {
  if (dir.empty()) return 0;
  std::error_code ec;
  if (!ghc::filesystem::is_directory(dir, ec)) return 0;
  int added = 0;
  for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
    ghc::filesystem::path p = e.path();
    ghc::filesystem::path md;
    std::string name;
    if (ghc::filesystem::is_directory(p, ec)) {
      if (ghc::filesystem::exists(p / "SKILL.md", ec)) md = p / "SKILL.md";
      else if (ghc::filesystem::exists(p / "skill.md", ec)) md = p / "skill.md";
      else continue;
      name = p.filename().string();
    } else if (p.extension() == ".md") {
      md = p;
      name = p.stem().string();
    } else {
      continue;
    }
    std::string text = read_text(md);
    if (text.empty()) continue;
    Skill s;
    std::string fm_name = front_value(text, "name");
    s.name = fm_name.empty() ? name : fm_name;
    if (has(s.name)) continue;  // 已有的不重复导入
    s.title_zh = front_value(text, "title");
    if (s.title_zh.empty()) s.title_zh = s.name;
    s.title_en = s.title_zh;
    s.desc_zh = front_value(text, "description");
    s.desc_en = s.desc_zh;
    s.body_zh = strip_frontmatter(text);
    s.body_en = s.body_zh;
    s.loadable = true;
    s.dir = md.parent_path().string();
    s.user = true;
    s.enabled = true;
    skills_.push_back(s);
    ++added;
  }
  return added;
}

bool SkillRegistry::add(const Skill& s) {
  if (s.name.empty()) return false;
  for (auto& e : skills_) {
    if (e.name == s.name) {
      e = s;
      e.user = true;
      return true;
    }
  }
  Skill n = s;
  n.user = true;
  skills_.push_back(n);
  return true;
}

bool SkillRegistry::set_enabled(const std::string& name, bool enabled) {
  for (auto& s : skills_) {
    if (s.name == name) {
      s.enabled = enabled;
      return true;
    }
  }
  return false;
}

bool SkillRegistry::remove(const std::string& name) {
  for (size_t i = 0; i < skills_.size(); ++i) {
    if (skills_[i].name == name) {
      // 内置 skill 删不掉（它们是代码的一部分），只能停用
      if (!skills_[i].user) return false;
      skills_.erase(skills_.begin() + static_cast<long>(i));
      return true;
    }
  }
  return false;
}

Json SkillRegistry::user_json() const {
  Json arr = Json::array();
  for (auto& s : skills_) {
    if (!s.user) continue;
    Json j = Json::object();
    j["name"] = s.name;
    j["title"] = s.title_zh;
    j["description"] = s.desc_zh;
    j["body"] = s.body_zh;
    j["dir"] = s.dir;
    j["enabled"] = s.enabled;
    arr.push_back(j);
  }
  return arr;
}

void SkillRegistry::load_user(const Json& j) {
  // 先摘掉旧的用户条目（重新加载 = 以磁盘上的设置为准）
  for (long i = static_cast<long>(skills_.size()) - 1; i >= 0; --i) {
    if (skills_[static_cast<size_t>(i)].user) {
      skills_.erase(skills_.begin() + i);
    }
  }
  if (!j.is_array()) return;
  for (size_t i = 0; i < j.size(); ++i) {
    if (!j[i].is_object()) continue;
    Skill s;
    s.name = jsonutil::get<std::string>(j[i], "name", "");
    if (s.name.empty()) continue;
    s.title_zh = jsonutil::get<std::string>(j[i], "title", s.name);
    s.title_en = s.title_zh;
    s.desc_zh = jsonutil::get<std::string>(j[i], "description", "");
    s.desc_en = s.desc_zh;
    s.body_zh = jsonutil::get<std::string>(j[i], "body", "");
    s.body_en = s.body_zh;
    s.dir = jsonutil::get<std::string>(j[i], "dir", "");
    s.enabled = jsonutil::get<bool>(j[i], "enabled", true);
    s.loadable = true;
    s.user = true;
    skills_.push_back(s);
  }
}

void register_skill_tools(ToolRegistry* registry, SkillRegistry* skills) {
  // load_skill：按需加载 skill 正文（addon2：不直接提供，按需加载）
  {
    ToolDef def;
    def.name = "load_skill";
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
    registry->register_tool("load_skill", def,
      [skills](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string name = jsonutil::get<std::string>(args, "name", "");
        if (name.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty skill name");
        return skills->load(name, parse_think_lang(ctx.thinking_lang));
      });
  }

  // list_skills：列出可用 skill 简述
  {
    ToolDef def;
    def.name = "list_skills";
    def.description = "列出可用 skill. List available skills (bilingual).";
    Json params = Json::object();
    params["type"] = "object";
    params["properties"] = Json::object();
    def.parameters = params;
    registry->register_tool("list_skills", def,
      [skills](const ToolContext& ctx, const Json&) -> FsResult {
        return FsResult::success(skills->list_json(parse_think_lang(ctx.thinking_lang)));
      });
  }
}

}  // namespace fs