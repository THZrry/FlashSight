// provider_types.cpp - enum/json 映射（无异常，宽容）
#include "provider_types.hpp"

namespace fs {

const char* api_kind_str(ApiKind k) {
  switch (k) {
    case ApiKind::kOpenAIChat: return "openai-chat";
    case ApiKind::kOpenAIResponse: return "openai-response";
    case ApiKind::kAnthropic: return "anthropic";
    case ApiKind::kCustom: return "custom";
    case ApiKind::kPlan: return "plan";  // legacy value still readable on disk
  }
  return "openai-chat";
}

const char* api_kind_display_str(ApiKind k) {
  // legacy "plan" is really "openai-chat plus a session header"; show it as the
  // shape it actually is so the UI never presents opencode as a request format
  return k == ApiKind::kPlan ? "openai-chat" : api_kind_str(k);
}

bool api_kind_from_str(const std::string& s, ApiKind* out) {
  if (!out) return false;
  if (s == "openai-chat") { *out = ApiKind::kOpenAIChat; return true; }
  if (s == "openai-response") { *out = ApiKind::kOpenAIResponse; return true; }
  if (s == "anthropic") { *out = ApiKind::kAnthropic; return true; }
  if (s == "custom") { *out = ApiKind::kCustom; return true; }
  if (s == "plan") { *out = ApiKind::kPlan; return true; }
  return false;
}

Json ModelEntry::to_json() const {
  Json j = Json::object();
  j["id"] = id;
  j["display"] = display.empty() ? id : display;
  Json c = Json::object();
  c["vision"] = caps.vision;
  c["thinking"] = caps.thinking;
  c["toolUse"] = caps.tool_use;
  c["contextLen"] = caps.context_len;
  j["caps"] = c;
  j["fromPreset"] = from_preset;
  j["fromApi"] = from_api;
  return j;
}

ModelEntry ModelEntry::from_json(const Json& j) {
  ModelEntry m;
  m.id = jsonutil::get<std::string>(j, "id", "");
  m.display = jsonutil::get<std::string>(j, "display", m.id);
  Json c;
  if (j.contains("caps") && j["caps"].is_object()) c = j["caps"];
  m.caps.vision = jsonutil::get<bool>(c, "vision", false);
  m.caps.thinking = jsonutil::get<bool>(c, "thinking", false);
  m.caps.tool_use = jsonutil::get<bool>(c, "toolUse", false);
  m.caps.context_len = jsonutil::get_int(c, "contextLen", 0);
  m.from_preset = jsonutil::get<bool>(j, "fromPreset", false);
  m.from_api = jsonutil::get<bool>(j, "fromApi", false);
  return m;
}

Json ProviderConn::to_json() const {
  Json j = Json::object();
  j["id"] = id;
  j["name"] = name;
  // legacy kPlan is reported as the shape it really is (openai-chat)
  j["kind"] = api_kind_display_str(kind);
  j["baseUrl"] = base_url;
  // api_key 不写入 to_json（避免误泄漏到日志/事件）；持久化用 to_json_persist
  j["enabled"] = enabled;
  j["sendSession"] = send_session || kind == ApiKind::kPlan;
  j["hasQuota"] = has_quota;
  j["quotaValue"] = quota_value;
  j["quotaKind"] = quota_kind;
  Json h = Json::object();
  for (auto& kv : custom_headers) h[kv.first] = kv.second;
  j["customHeaders"] = h;
  return j;
}

ProviderConn ProviderConn::from_json(const Json& j) {
  ProviderConn p;
  p.id = jsonutil::get<std::string>(j, "id", "");
  p.name = jsonutil::get<std::string>(j, "name", "");
  ApiKind k = ApiKind::kOpenAIChat;
  api_kind_from_str(jsonutil::get<std::string>(j, "kind", "openai-chat"), &k);
  // legacy "plan" configs: read back as a chat provider that sends the session
  // header, so an old file keeps working without a migration step
  p.send_session = jsonutil::get<bool>(j, "sendSession", k == ApiKind::kPlan);
  p.kind = (k == ApiKind::kPlan) ? ApiKind::kOpenAIChat : k;
  p.base_url = jsonutil::get<std::string>(j, "baseUrl", "");
  p.api_key = jsonutil::get<std::string>(j, "apiKey", "");
  p.enabled = jsonutil::get<bool>(j, "enabled", true);
  p.has_quota = jsonutil::get<bool>(j, "hasQuota", false);
  p.quota_value = jsonutil::get_number(j, "quotaValue", 0.0);
  p.quota_kind = jsonutil::get<std::string>(j, "quotaKind", "unknown");
  if (j.contains("customHeaders") && j["customHeaders"].is_object()) {
    for (auto it = j["customHeaders"].begin(); it != j["customHeaders"].end(); ++it) {
      if (it.value().is_string()) p.custom_headers[it.key()] = it.value().get<std::string>();
    }
  }
  return p;
}

}  // namespace fs