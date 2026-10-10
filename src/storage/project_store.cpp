// project_store.cpp - projects.json registry
#include "project_store.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

#include "fs_json.hpp"
#include "fs_path.hpp"
#include "fs_random.hpp"

namespace fs {

// 用注入时钟存引用（生产：WallClock）。简单起见这里内部用 time()（同 WallClock）。
namespace {
// avoid unused warning
inline int64_t now_for_registry() { return static_cast<int64_t>(std::time(nullptr)); }
}  // namespace

void ProjectRegistry::load() {
  std::ifstream ifs(file_.string(), std::ios::binary);
  if (!ifs) return;
  try {
    nlohmann::json j = nlohmann::json::parse(ifs);
    if (j.is_object()) doc_ = std::move(j);
  } catch (...) {
    // 坏文件：弃用，内容留给 save 覆盖
  }
}

std::string ProjectRegistry::gen_pid() {
  static XorShiftRandom rng(0);
  std::string id;
  do {
    id = "p_" + rng.hex(4);
  } while (doc_.find(id) != doc_.end());
  return id;
}

std::string ProjectRegistry::find_by_path(const std::string& path) const {
  ghc::filesystem::path norm = pathutil::normalize(path);
  std::string ns = norm.string();
  for (auto it = doc_.begin(); it != doc_.end(); ++it) {
    if (!it.value().is_object()) continue;
    auto p = it.value().find("path");
    if (p == it.value().end() || !p->is_string()) continue;
    std::string other = pathutil::normalize(it.value()["path"].get<std::string>()).string();
    if (ns == other) {
      return it.key();
    }
  }
  return "";
}

std::string ProjectRegistry::register_project(const std::string& path, const std::string& pid) {
  std::string norm = pathutil::normalize(path).string();
  std::string existing = find_by_path(norm);
  if (!existing.empty()) {
    doc_[existing]["lastActive"] = now_for_registry();
    return existing;
  }
  std::string id = pid.empty() ? gen_pid() : pid;
  nlohmann::json p = nlohmann::json::object();
  p["path"] = norm;
  p["lastSession"] = "";
  p["lastActive"] = now_for_registry();
  doc_[id] = p;
  return id;
}

nlohmann::json ProjectRegistry::get(const std::string& pid) const {
  auto it = doc_.find(pid);
  if (it == doc_.end()) return nlohmann::json();
  return it.value();
}

nlohmann::json ProjectRegistry::list() const {
  nlohmann::json arr = nlohmann::json::array();
  for (auto it = doc_.begin(); it != doc_.end(); ++it) {
    if (!it.value().is_object()) continue;
    nlohmann::json e = it.value();
    e["pid"] = it.key();
    arr.push_back(e);
  }
  std::sort(arr.begin(), arr.end(),
            [](const nlohmann::json& a, const nlohmann::json& b) {
              int64_t ta = jsonutil::get_int(a, "lastActive", 0);
              int64_t tb = jsonutil::get_int(b, "lastActive", 0);
              return ta > tb;
            });
  return arr;
}

void ProjectRegistry::set_last_session(const std::string& path, const std::string& sid) {
  std::string pid = find_by_path(path);
  if (pid.empty()) return;
  doc_[pid]["lastSession"] = sid;
  doc_[pid]["lastActive"] = now_for_registry();
  std::string err;
  save(&err);
}

bool ProjectRegistry::remove(const std::string& pid) {
  if (doc_.erase(pid) == 0) return false;
  std::string err;
  return save(&err);
}

int64_t ProjectRegistry::count() const { return static_cast<int64_t>(doc_.size()); }

bool ProjectRegistry::save(std::string* err) {
  return jsonutil::write_atomic(file_.string(), doc_, err);
}

}  // namespace fs