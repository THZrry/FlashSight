// fs_store.cpp - resolve global & project file layout
#include "fs_store.hpp"

#include "fs_path.hpp"

namespace fs {

GlobalPaths GlobalPaths::resolve() {
  GlobalPaths gp;
  gp.root = pathutil::global_dir();
  gp.config = gp.root / "config.json";
  gp.providers = gp.root / "providers.json";
  gp.projects = gp.root / "projects.json";
  gp.usage = gp.root / "usage" / "calls.jsonl";
  return gp;
}

ProjectStore ProjectStore::of(const ghc::filesystem::path& project_root) {
  ProjectStore ps;
  ps.project_root = pathutil::normalize(project_root);
  ps.data_dir = ps.project_root / ".flashsight";
  ps.project_json = ps.data_dir / "project.json";
  ps.sessions_dir = ps.data_dir / "sessions";
  return ps;
}

ghc::filesystem::path ProjectStore::session_dir(const std::string& sid) const {
  return sessions_dir / sid;
}
ghc::filesystem::path ProjectStore::full_json(const std::string& sid) const {
  return session_dir(sid) / "full.json";
}
ghc::filesystem::path ProjectStore::runtime_json(const std::string& sid) const {
  return session_dir(sid) / "runtime.json";
}
ghc::filesystem::path ProjectStore::patches_dir(const std::string& sid) const {
  return session_dir(sid) / "patches";
}
ghc::filesystem::path ProjectStore::patch(const std::string& sid,
                                          const std::string& turn_id) const {
  return patches_dir(sid) / (turn_id + ".diff");
}
ghc::filesystem::path ProjectStore::terminals_dir(const std::string& sid) const {
  return session_dir(sid) / "terminals";
}
ghc::filesystem::path ProjectStore::terminal_log(const std::string& sid,
                                                 const std::string& tid) const {
  return terminals_dir(sid) / (tid + ".log");
}

}  // namespace fs