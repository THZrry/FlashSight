// skill_registry.hpp - 中英双语 skill 注册表（addon2.md）
// 工具/能力以"skill"形式提供简明清晰的中英双语说明，由设置键 model.thinkingLang
// 切换输出语言（auto/zh/en）。部分能力（浏览器控制等）不直接暴露为工具，
// 而是做成 skill 由 LLM 按需加载（addon2：不直接提供，用 /browser-load 等）。
#pragma once

#include <string>
#include <vector>

#include "fs_json.hpp"
#include "tool_registry.hpp"

namespace fs {

// 思考语言（设置键 model.thinkingLang）
enum class ThinkLang { kAuto = 0, kZh, kEn };

const char* think_lang_str(ThinkLang l);
// 解析设置值："auto"|"zh"|"en"（未知 → auto）
ThinkLang parse_think_lang(const std::string& s);
// 有效语言（auto → 跟随 i18n 系统语言）
ThinkLang effective_think_lang(ThinkLang l);

// 一条 skill：名称 + 中英标题 + 中英说明 + 正文（加载后给模型）
struct Skill {
  std::string name;         // 如 browser-control
  std::string title_zh;
  std::string title_en;
  std::string desc_zh;      // 一行简述（供工具列表/提示用）
  std::string desc_en;
  std::string body_zh;      // 加载后返回的详细用法
  std::string body_en;
  bool loadable = true;     // true=需按需加载（不进工具列表）
  std::string dir;          // 来源目录（用户导入的 skill 才有；卡片上显示）
  bool user = false;        // 用户导入/新建（可删除）
  bool enabled = true;      // 停用只是不让模型看到，用户仍可 /名称 调用
};

// 内置 skill 表
std::vector<Skill> builtin_skills();

class SkillRegistry {
 public:
  SkillRegistry();

  // 按名加载（返回该语言正文；不存在返回 kNotFound）
  FsResult load(const std::string& name, ThinkLang lang);
  // 一行简述（列表用）
  FsResult brief(const std::string& name, ThinkLang lang);
  // 全部 skill 的简述列表（按语言）
  Json list_json(ThinkLang lang) const;
  bool has(const std::string& name) const;

  // ----- 用户 skill（导入 / 新建 / 启停 / 删除）-----
  // 从一个目录导入：该目录下每个含 SKILL.md（或 <name>.md）的子目录/文件算一条。
  // 返回新增条数。
  int import_dir(const std::string& dir);
  // 新建一条（正文即用户填的说明）。
  bool add(const Skill& s);
  bool set_enabled(const std::string& name, bool enabled);
  bool remove(const std::string& name);
  // 用户 skill 的整份进出（持久化到设置 skills.entries）。
  Json user_json() const;
  void load_user(const Json& j);

 private:
  std::vector<Skill> skills_;
};

// 注册 skill 工具（tool.skill.load / tool.skill.list）
void register_skill_tools(ToolRegistry* registry, SkillRegistry* skills);

}  // namespace fs