// provider_store.cpp - providers.json 读写
#include "provider_store.hpp"

#include <fstream>
#include <sstream>

#include "fs_random.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs {

ProviderStore::ProviderStore(const ghc::filesystem::path& providers_file)
    : file_(providers_file.string()) {
  load();
}

void ProviderStore::load() {
  std::ifstream ifs(file_, std::ios::binary);
  if (!ifs) return;
  std::stringstream ss;
  ss << ifs.rdbuf();
  try {
    Json j = Json::parse(ss.str());
    if (j.is_array()) doc_ = j;
  } catch (...) {
    doc_ = Json::array();  // 坏文件：空表起步
  }
}

bool ProviderStore::save(std::string* err) const {
  bool ok = jsonutil::write_atomic(file_, doc_, err);
  if (ok) {
#if !defined(_WIN32)
    ::chmod(file_.c_str(), S_IRUSR | S_IWUSR);  // 600
#endif
  }
  return ok;
}

std::vector<ProviderConn> ProviderStore::list() const {
  std::vector<ProviderConn> out;
  if (!doc_.is_array()) return out;
  for (auto& e : doc_) {
    if (e.is_object()) out.push_back(ProviderConn::from_json(e));
  }
  return out;
}

ProviderConn ProviderStore::get(const std::string& id) const {
  if (!doc_.is_array()) return ProviderConn();
  for (auto& e : doc_) {
    if (e.is_object() && jsonutil::get<std::string>(e, "id", "") == id) {
      return ProviderConn::from_json(e);
    }
  }
  return ProviderConn();
}

std::string ProviderStore::upsert(const ProviderConn& conn) {
  if (!doc_.is_array()) doc_ = Json::array();
  ProviderConn c = conn;
  if (c.id.empty()) {
    XorShiftRandom rng(0);
    c.id = "pr_" + rng.hex(4);
  }
  Json cj = c.to_json();
  cj["apiKey"] = c.api_key;  // 显式补回（to_json 不带密钥）
  for (auto& e : doc_) {
    if (e.is_object() && jsonutil::get<std::string>(e, "id", "") == c.id) {
      e = cj;
      return c.id;
    }
  }
  doc_.push_back(cj);
  return c.id;
}

bool ProviderStore::remove(const std::string& id) {
  if (!doc_.is_array()) return false;
  for (auto it = doc_.begin(); it != doc_.end(); ++it) {
    if (it->is_object() && jsonutil::get<std::string>(*it, "id", "") == id) {
      doc_.erase(it);
      return true;
    }
  }
  return false;
}

}  // namespace fs