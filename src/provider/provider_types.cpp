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
  c["audio"] = caps.audio;
  c["video"] = caps.video;
  c["pdf"] = caps.pdf;
  c["structuredOutput"] = caps.structured_output;
  c["maxOutput"] = caps.max_output;
  j["caps"] = c;
  j["fromPreset"] = from_preset;
  j["fromApi"] = from_api;
  j["enabled"] = enabled;
  j["userSet"] = user_set;
  if (!icon.empty()) j["icon"] = icon;
  // thinking: only emit when the model actually has levels, so a model without
  // configurable reasoning does not carry an empty array the UI would render
  // as a control with no options.
  if (!thinking.levels.empty()) {
    Json t = Json::object();
    Json lv = Json::array();
    for (auto& l : thinking.levels) lv.push_back(l);
    t["levels"] = lv;
    t["default"] = thinking.default_level;
    j["thinking"] = t;
  }
  if (retry.max > 0 || retry.backoff_ms > 0) {
    Json r = Json::object();
    r["max"] = retry.max;
    r["backoffMs"] = retry.backoff_ms;
    j["retry"] = r;
  }
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
  m.caps.audio = jsonutil::get<bool>(c, "audio", false);
  m.caps.video = jsonutil::get<bool>(c, "video", false);
  m.caps.pdf = jsonutil::get<bool>(c, "pdf", false);
  m.caps.structured_output = jsonutil::get<bool>(c, "structuredOutput", false);
  m.caps.max_output = jsonutil::get_int(c, "maxOutput", 0);
  m.from_preset = jsonutil::get<bool>(j, "fromPreset", false);
  m.from_api = jsonutil::get<bool>(j, "fromApi", false);
  m.enabled = jsonutil::get<bool>(j, "enabled", true);
  m.user_set = jsonutil::get<bool>(j, "userSet", false);
  m.icon = jsonutil::get<std::string>(j, "icon", "");
  if (j.contains("thinking") && j["thinking"].is_object()) {
    const Json& t = j["thinking"];
    if (t.contains("levels") && t["levels"].is_array()) {
      for (auto& l : t["levels"]) {
        if (l.is_string()) m.thinking.levels.push_back(l.get<std::string>());
      }
    }
    m.thinking.default_level = jsonutil::get<std::string>(t, "default", "");
  }
  if (j.contains("retry") && j["retry"].is_object()) {
    m.retry.max = static_cast<int>(jsonutil::get_int(j["retry"], "max", 0));
    m.retry.backoff_ms =
        static_cast<int>(jsonutil::get_int(j["retry"], "backoffMs", 0));
  }
  return m;
}


Json ProviderConn::to_json() const {
  Json j = Json::object();
  j["id"] = id;
  j["name"] = name;
  // legacy kPlan is reported as the shape it really is (openai-chat)
  j["kind"] = api_kind_display_str(kind);
  j["baseUrl"] = base_url;
  j["oauth"] = oauth;
  // api_key 不写入 to_json（避免误泄漏到日志/事件）；持久化用 to_json_persist
  j["enabled"] = enabled;
  j["sendSession"] = send_session || kind == ApiKind::kPlan;
  j["hasQuota"] = has_quota;
  j["quotaValue"] = quota_value;
  j["quotaKind"] = quota_kind;
  Json h = Json::object();
  for (auto& kv : custom_headers) h[kv.first] = kv.second;
  j["customHeaders"] = h;
  if (body_override.is_object() && !body_override.empty()) {
    j["bodyOverride"] = body_override;
  }
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
    p.oauth = jsonutil::get<bool>(j, "oauth", false);
    p.enabled = jsonutil::get<bool>(j, "enabled", true);
  p.has_quota = jsonutil::get<bool>(j, "hasQuota", false);
  p.quota_value = jsonutil::get_number(j, "quotaValue", 0.0);
  p.quota_kind = jsonutil::get<std::string>(j, "quotaKind", "unknown");
  if (j.contains("customHeaders") && j["customHeaders"].is_object()) {
    for (auto it = j["customHeaders"].begin(); it != j["customHeaders"].end(); ++it) {
      if (it.value().is_string()) p.custom_headers[it.key()] = it.value().get<std::string>();
    }
  }
  if (j.contains("bodyOverride") && j["bodyOverride"].is_object()) {
    p.body_override = j["bodyOverride"];
  }
  return p;
}

}  // namespace fs