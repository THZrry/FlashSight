// deny_list.cpp - deny list matching
#include "deny_list.hpp"

#include "fs_path.hpp"

namespace fs {

void DenyList::set_blacklist(const std::vector<std::string>& paths) {
  black_.clear();
  for (auto& s : paths) black_.push_back(ghc::filesystem::path(s));
}
void DenyList::set_whitelist(const std::vector<std::string>& paths) {
  white_.clear();
  for (auto& s : paths) white_.push_back(ghc::filesystem::path(s));
}
std::vector<std::string> DenyList::blacklist() const {
  std::vector<std::string> out;
  for (auto& p : black_) out.push_back(p.string());
  return out;
}
std::vector<std::string> DenyList::whitelist() const {
  std::vector<std::string> out;
  for (auto& p : white_) out.push_back(p.string());
  return out;
}

bool DenyList::path_matches(const ghc::filesystem::path& p,
                           const std::vector<ghc::filesystem::path>& list,
                           std::string* matched) const {
  for (auto& entry : list) {
    if (entry.empty()) continue;
    ghc::filesystem::path e = pathutil::normalize(entry);
    ghc::filesystem::path np = pathutil::normalize(p);
    if (pathutil::is_within(np, e)) {
      if (matched) *matched = e.string();
      return true;
    }
  }
  return false;
}

PathMatch DenyList::match(const ghc::filesystem::path& p) const {
  PathMatch m;
  // 黑名单优先
  {
    std::string hit;
    if (path_matches(pathutil::normalize(p), black_, &hit)) {
      m.denied = true;
      m.matched = hit;
      return m;
    }
  }
  // 白名单
  {
    std::string hit;
    if (path_matches(pathutil::normalize(p), white_, &hit)) {
      m.whitelist_allow = true;
      m.matched = hit;
      return m;
    }
  }
  return m;
}

void DenyList::from_json(const Json& json) {
  std::vector<std::string> b, w;
  if (json.contains("blacklist")) {
    for (auto& e : json["blacklist"]) {  // expect array of strings
      if (e.is_string()) b.push_back(e.get<std::string>());
    }
  }
  if (json.contains("whitelist")) {
    for (auto& e : json["whitelist"]) {
      if (e.is_string()) w.push_back(e.get<std::string>());
    }
  }
  set_blacklist(b);
  set_whitelist(w);
}

}  // namespace fs