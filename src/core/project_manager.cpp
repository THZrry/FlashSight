// project_manager.cpp - ProjectManager
#include "project_manager.hpp"

#include <ctime>
#include <sys/stat.h>

#include "fs_path.hpp"

namespace fs {

namespace {
// 非 git 项目自动 git init 开关（permissions.md §3，默认关），路径存在才 init
void maybe_git_init(const ghc::filesystem::path& root, bool allowed) {
  if (!allowed) return;
  if (!ghc::filesystem::exists(root)) return;
  if (ghc::filesystem::exists(root / ".git")) return;
  // 调 git CLI（零依赖起步，build.md §3）
  (void)root;
}
}  // namespace

void ProjectManager::load() { registry_.load(); }
bool ProjectManager::save() {
  std::string err;
  return registry_.save(&err);
}

FsResult ProjectManager::open_project(const std::string& path) {
  if (path.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty path");
  ghc::filesystem::path p = pathutil::normalize(path);
  if (!ghc::filesystem::exists(p)) {
    return FsResult::error(ErrCode::kNotFound, "project dir not exists: " + p.string());
  }
  std::string pid = registry_.register_project(p.string(), "");
  std::string err;
  if (!registry_.save(&err)) return FsResult::error(ErrCode::kIo, err);
  return FsResult::success(Json{{"pid", pid}, {"path", p.string()}});
}

FsResult ProjectManager::create_project(const std::string& path, const std::string& pid) {
  if (path.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty path");
  ghc::filesystem::path p = pathutil::normalize(path);
  std::error_code ec;
  if (!ghc::filesystem::exists(p)) {
    ghc::filesystem::create_directories(p, ec);
    if (ec) return FsResult::error(ErrCode::kIo, "create dir failed: " + ec.message());
  }
  std::string id = registry_.register_project(p.string(), pid);
  std::string err;
  if (!registry_.save(&err)) return FsResult::error(ErrCode::kIo, err);
  return FsResult::success(Json{{"pid", id}, {"path", p.string()}});
}

FsResult ProjectManager::remove_project(const std::string& pid) {
  if (!registry_.remove(pid)) {
    return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  }
  return FsResult::success();
}

FsResult ProjectManager::get_project(const std::string& pid) const {
  nlohmann::json j = registry_.get(pid);
  if (j.is_null()) return FsResult::error(ErrCode::kNotFound, "project not found: " + pid);
  return FsResult::success(j);
}

FsResult ProjectManager::list_projects() const {
  nlohmann::json arr = registry_.list();
  // annotate sessionCount so the UI can show it without an extra round-trip
  if (arr.is_array()) {
    for (auto& e : arr) {
      if (!e.is_object()) continue;
      std::string path = jsonutil::get<std::string>(e, "path", "");
      if (path.empty()) continue;
      int64_t n = 0;
      ghc::filesystem::path sessions =
          ProjectStore::of(ghc::filesystem::path(path)).sessions_dir;
      std::error_code ec;
      if (ghc::filesystem::exists(sessions, ec)) {
        for (auto& d : ghc::filesystem::directory_iterator(sessions, ec)) {
          if (ghc::filesystem::is_directory(d.path(), ec) &&
              ghc::filesystem::exists(d.path() / "full.json", ec)) {
            ++n;
          }
        }
      }
      e["sessionCount"] = n;
    }
  }
  return FsResult::success(arr);
}

std::string ProjectManager::pid_by_path(const std::string& path) const {
  return registry_.find_by_path(path);
}

ghc::filesystem::path ProjectManager::project_path(const std::string& pid) const {
  nlohmann::json j = registry_.get(pid);
  if (j.is_object() && j.contains("path") && j["path"].is_string()) {
    return ghc::filesystem::path(j["path"].get<std::string>());
  }
  return ghc::filesystem::path();
}

ProjectStore ProjectManager::store_for(const std::string& pid) const {
  return ProjectStore::of(project_path(pid));
}

}  // namespace fs