// provider_store.hpp - providers.json 读写 + 密钥管理（provider.md §2；600 权限；TODO 钥匙串）
#pragma once

#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"
#include "provider_types.hpp"

namespace fs {

// 连接增删改查（FsApi: provider.listProviders / testConnection）
class ProviderStore {
 public:
  explicit ProviderStore(const ghc::filesystem::path& providers_file);

  // 载入 providers.json（坏文件宽容：空表起步）
  void load();
  // 原子持久化（含密钥，前期明文；POSIX 下尝试 chmod 600）
  bool save(std::string* err) const;

  // 增删改查（改走 upsert：读 → 改 → upsert → save）
  std::vector<ProviderConn> list() const;
  // 按 id 查找（未找到返回空值：id 为空）
  ProviderConn get(const std::string& id) const;
  // upsert（id 为空则生成）；返回最终 id
  std::string upsert(const ProviderConn& conn);
  bool remove(const std::string& id);

  // 落盘路径（暴露给 UI/诊断）
  const std::string& file_path() const { return file_; }

 private:
  std::string file_;
  Json doc_ = Json::array();  // [ {conn...}, ... ]
};

}  // namespace fs