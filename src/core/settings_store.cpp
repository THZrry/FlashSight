// settings_store.cpp - two-level settings store
#include "settings_store.hpp"

#include "fs_json.hpp"
#include "fs_path.hpp"

#include "fs_i18n.hpp"
#include "fs_log.hpp"
#include "fs_path.hpp"
#include "settings_store.hpp"
#include "storage/fs_store.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs {

SettingsStore::SettingsStore(const ghc::filesystem::path& global_dir,
                            const ghc::filesystem::path& project_dir,
                            bool allow_global_read)
    : global_dir_(global_dir), project_dir_(project_dir),
      allow_global_read_(allow_global_read) {
  global_doc_.path = global_dir / "config.json";
  project_doc_.path = project_dir / "project.json";
  global_ = &global_doc_;
  project_ = &project_doc_;
  load();
}

ghc::filesystem::path SettingsStore::global_file() const { return global_dir_ / "config.json"; }
ghc::filesystem::path SettingsStore::project_file() const { return project_dir_ / "project.json"; }

void SettingsStore::load_doc(Doc& d) {
  std::ifstream ifs(d.path.string(), std::ios::binary);
  if (!ifs) return;
  std::stringstream ss;
  ss << ifs.rdbuf();
  try {
    nlohmann::json j = nlohmann::json::parse(ss.str());
    if (j.is_object()) d.doc = std::move(j);
  } catch (...) {
    log::warn("settings load parse failed: " + d.path.string());
  }
  if (d.doc.count("ignoreGlobal")) {
    ignore_global_ = d.doc["ignoreGlobal"].is_boolean() && d.doc["ignoreGlobal"].get<bool>();
  }
}

void SettingsStore::load() {
  load_doc(global_doc_);
  load_doc(project_doc_);
  sync_language();
}

// ui.language 落盘后立即生效（重启也要生效，所以在 load 之后也调一次）
void SettingsStore::sync_language() {
  if (!allow_global_read_) return;  // 单测里不碰全局 i18n 状态
  std::string lang = "auto";
  auto it = global_->doc.find(kKeyUiLang);
  if (it != global_->doc.end() && it->is_string()) lang = it->get<std::string>();
  i18n::set_lang(i18n::lang_from_setting(lang));
}

FsResult SettingsStore::persist(Doc& d) {
  std::string err;
  if (!jsonutil::write_atomic(d.path.string(), d.doc, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

SettingValue SettingsStore::get(const std::string& key) const {
  SettingValue sv;
  // project first (unless ignoreGlobal or project empty)
  auto pit = project_->doc.find(key);
  if (pit != project_->doc.end()) {
    sv.value = *pit;
    sv.source = "project";
    sv.project_set = true;
    return sv;
  }
  if (!ignore_global_ && allow_global_read_) {
    auto git = global_->doc.find(key);
    if (git != global_->doc.end()) {
      sv.value = *git;
      sv.source = "global";
      sv.global_set = true;
      return sv;
    }
  }
  sv.source = "merged-default";
  // defaults
  if (key == kKeyNewSessionFilePerm) sv.value = "permissive";
  else if (key == kKeyNewSessionAutoMode) sv.value = false;
  else if (key == kKeyNewSessionEnterSend) sv.value = "enter";
  else if (key == kKeyPermNetworkEnabled) sv.value = true;
  else if (key == kKeyPermNetworkVerifyCert) sv.value = true;
  else if (key == kKeyPermTerminalEnabled) sv.value = true;
  else if (key == kKeyPermAutoGitInit) sv.value = false;
  else if (key == kKeyTerminalShell) sv.value = "";   // 空=自动探测
  else if (key == kKeyTerminalEncoding) sv.value = ""; // 空=系统默认
  else if (key == kKeyUiLang) sv.value = "auto";
  else if (key == kKeyUiTheme) sv.value = "default";
  else if (key == kKeyUiMode) sv.value = "auto";
  else if (key == kKeyUiCodeHighlight) sv.value = true;
  else if (key == kKeyUiTabs) sv.value = "top";
  else if (key == kKeyUiTabTooltip) sv.value = true;
  else if (key == kKeyUiMaxTabs) sv.value = 8;
  else if (key == kKeyBrowserEnabled) sv.value = false;
  else if (key == kKeyComputerEnabled) sv.value = false;
  else if (key == kKeyComputerScreenshot) sv.value = true;
  else if (key == kKeyUsageRetainDays) sv.value = 90;
  else if (key == kKeyHistoryRecentTurns) sv.value = 20;
  else if (key == kKeyHistoryTokenBudget) sv.value = 8000;
  else if (key == kKeyOverfitEnabled) sv.value = false;
  else if (key == kKeySubagentMaxParallel) sv.value = 4;
  else if (key == kKeySubagentMaxDepth) sv.value = 3;
  else if (key == kKeyThinkingLang) sv.value = "auto";
  else if (key == kKeyLspServers) sv.value = nlohmann::json::object();
  else sv.value = nlohmann::json();
  return sv;
}

nlohmann::json SettingsStore::get_value(const std::string& key,
                                        const nlohmann::json& def) const {
  SettingValue sv = get(key);
  return sv.value.is_null() ? def : sv.value;
}

std::map<std::string, std::string> SettingsStore::merged_env() const {
  std::map<std::string, std::string> out;
  auto fill = [&out](const nlohmann::json& doc) {
    auto it = doc.find(kKeyEnvVars);
    if (it == doc.end() || !it->is_object()) return;
    for (auto vit = it->begin(); vit != it->end(); ++vit) {
      // 支持两种形态：{value} 或纯字符串
      if (vit.value().is_object() && vit.value().contains("value") &&
          vit.value()["value"].is_string()) {
        out[vit.key()] = vit.value()["value"].get<std::string>();
      } else if (vit.value().is_string()) {
        out[vit.key()] = vit.value().get<std::string>();
      }
    }
  };
  if (!ignore_global_ && allow_global_read_) fill(global_->doc);
  fill(project_->doc);  // 项目覆盖全局
  return out;
}

std::map<std::string, std::string> SettingsStore::merged_cli_tools() const {
  std::map<std::string, std::string> out;
  auto fill = [&out](const nlohmann::json& doc) {
    auto it = doc.find(kKeyCliTools);
    if (it == doc.end() || !it->is_object()) return;
    for (auto vit = it->begin(); vit != it->end(); ++vit) {
      if (vit.value().is_object() && vit.value().contains("path") &&
          vit.value()["path"].is_string()) {
        out[vit.key()] = vit.value()["path"].get<std::string>();
      } else if (vit.value().is_string()) {
        out[vit.key()] = vit.value().get<std::string>();
      }
    }
  };
  if (!ignore_global_ && allow_global_read_) fill(global_->doc);
  fill(project_->doc);
  return out;
}

FsResult SettingsStore::set_global(const std::string& key,
                                   const nlohmann::json& value) {
  global_->doc[key] = value;
  if (key == kKeyUiLang && value.is_string()) {
    i18n::set_lang(i18n::lang_from_setting(value.get<std::string>()));
  }
  return persist(*global_);
}

FsResult SettingsStore::set_project(const std::string& key,
                                    const nlohmann::json& value) {
  project_->doc[key] = value;
  if (key == "ignoreGlobal" && value.is_boolean()) {
    ignore_global_ = value.get<bool>();
  }
  return persist(*project_);
}

FsResult SettingsStore::unset_global(const std::string& key) {
  global_->doc.erase(key);
  return persist(*global_);
}

FsResult SettingsStore::unset_project(const std::string& key) {
  if (project_->doc.erase(key) == 1) return persist(*project_);
  return FsResult::success();
}

FsResult SettingsStore::set_ignore_global(bool ignore) {
  ignore_global_ = ignore;
  project_->doc["ignoreGlobal"] = ignore;
  return persist(*project_);
}

FsResult SettingsStore::save_global() { return persist(*global_); }
FsResult SettingsStore::save_project() { return persist(*project_); }

std::vector<std::string> diff_keys(const nlohmann::json& a, const nlohmann::json& b) {
  std::vector<std::string> out;
  auto add = [&](const nlohmann::json& s, const nlohmann::json& t) {
    for (auto it = s.begin(); it != s.end(); ++it) {
      if (!t.contains(it.key()) || t.at(it.key()) != it.value()) {
        out.push_back(it.key());
      }
    }
  };
  add(a, b);
  add(b, a);
  return out;
}

}  // namespace fs