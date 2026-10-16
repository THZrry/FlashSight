// model_table.hpp - 模型表四级回退 + 能力标签（provider.md §3/§4）
#pragma once

#include <string>
#include <vector>

#include "fs_error.hpp"
#include "fs_net.hpp"
#include "model_naming.hpp"
#include "provider_types.hpp"

namespace fs {

// 为新建/来自 API 的模型补默认值（思考档位、图标），不覆盖用户已设的值
void fill_model_defaults(ModelEntry* m);

// 四级回退：用户自定义 > API 获取 > 预置列表 > 保守缺省
// 一键获取失败回退上一级并提示，不清空已有配置。
class ModelTable {
 public:
  // 预置表（内置常见模型名 + 能力标签；启动时灌入，用户可改）
  void set_preset(const std::vector<ModelEntry>& preset);
  // API 一键获取结果（按 provider 过滤存储）
  void set_api_models(const std::string& provider_id, const std::vector<ModelEntry>& models);
  // 用户自定义（最高优先；显式改能力标签）
  void set_user_model(const std::string& provider_id, const ModelEntry& m);
  // 删除用户自定义模型（预置/API 模型删不掉，只能停用）
  bool remove_user_model(const std::string& provider_id, const std::string& model_id);
  // 某 provider 的用户自定义模型
  std::vector<ModelEntry> user_models(const std::string& provider_id) const;

  // 持久化：用户模型设置整份进出（存 settings 的 provider.models 键）
  Json user_models_json() const;
  void load_user_models(const Json& j);

  // 某 provider 的完整模型列表（用户 + API，按 id 去重、用户优先），
  // 每一项都补齐默认档位/图标。两者都空时回退预置表（保持旧行为）。
  std::vector<ModelEntry> list_models(const std::string& provider_id) const;

  // 按优先级取：用户 > api > preset > 保守缺省
  // 保守缺省：tool_use=false（不支持 toolUse 拒绝进会话，provider.md §4）
  ModelEntry resolve(const std::string& provider_id, const std::string& model_id) const;

  // 一键获取（走 NetClient；失败回退上一级并提示）
  // 返回 {ok, models, fallbackLevel, message}
  struct FetchResult {
    bool ok = false;
    std::vector<ModelEntry> models;
    std::string fallback_level;  // "user"|"api"|"preset"|"default"
    std::string message;
  };
  FetchResult fetch_models(const std::string& provider_id, const ProviderConn& conn,
                           const NetClient& net) const;

  // 保守缺省（tool_use 恒 false）
  static ModelEntry conservative_default(const std::string& model_id);

 private:
  std::vector<ModelEntry> preset_;
  // provider_id -> api models
  std::vector<std::pair<std::string, std::vector<ModelEntry>>> api_;
  // provider_id -> user overrides
  std::vector<std::pair<std::string, std::vector<ModelEntry>>> user_;
};

// 内置预置表（常见模型名 + 能力；用户自定义 > API > 此表 > 保守缺省）
std::vector<ModelEntry> builtin_models_preset();

}  // namespace fs
