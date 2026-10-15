// provider_types.hpp - provider 层共享类型（provider.md §1–§4）
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {

// 调用形态（provider.md §1）
// NOTE: 这描述的是"请求长什么样"，与供应商品牌无关。opencode 之类并不是一种
// 形态——它只是 endpoint 不同、并多带一个 x-opencode-session 头，所以用
// send_session 标记表达，而不是单独一种 kind。
enum class ApiKind {
  kOpenAIChat,      // OpenAI Chat Completions（含自定义 endpoint + header）
  kOpenAIResponse,  // OpenAI Response API
  kAnthropic,       // Anthropic Messages
  kCustom,          // endpoint + 自定义 header（复用 chat 形态，仅 header 不同）
  kPlan,            // 兼容旧配置：读取时归一化为 kOpenAIChat + send_session
};

const char* api_kind_str(ApiKind k);
bool api_kind_from_str(const std::string& s, ApiKind* out);
// 供 UI 展示的形态名（kPlan 显示为 openai-chat）
const char* api_kind_display_str(ApiKind k);

// 模型能力标签（provider.md §4）
struct ModelCapabilities {
  bool vision = false;
  bool thinking = false;
  bool tool_use = false;   // 不支持 toolUse 的模型拒绝进会话
  int64_t context_len = 0; // 0 = 未知
};

// 模型条目（含能力标签）
struct ModelEntry {
  std::string id;       // 模型名（发给 API 的 model 字段）
  std::string display;  // UI 显示名（缺省 = id）
  ModelCapabilities caps;
  bool from_preset = false;  // 来自预置表
  bool from_api = false;      // 来自 API 一键获取

  Json to_json() const;
  static ModelEntry from_json(const Json& j);
};

// provider 连接（provider.md §1/§2：密钥前期明文 600 权限）
struct ProviderConn {
  std::string id;        // 稳定 id（配置引用键）
  std::string name;      // 显示名
  ApiKind kind = ApiKind::kOpenAIChat;
  std::string base_url;  // endpoint（如 https://api.openai.com/v1）
  std::string api_key;   // 前期明文；TODO 迁钥匙串
  std::map<std::string, std::string> custom_headers;  // 自定义 header（provider.md §5）
  bool enabled = true;
  // 发送会话亲和头 x-opencode-session（opencode 系套餐端点需要；与形态无关）
  bool send_session = false;

  // 套餐（quota）字段：三态（钱/额度/unknown），用量页成本表依赖（provider.md §1）
  bool has_quota = false;
  double quota_value = 0.0;
  std::string quota_kind;  // "money"|"quota"|"unknown"

  Json to_json() const;
  static ProviderConn from_json(const Json& j);
};

}  // namespace fs