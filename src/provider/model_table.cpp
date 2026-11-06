// model_table.cpp - 模型表四级回退实现
#include "model_table.hpp"

#include "model_naming.hpp"

#include "fs_net.hpp"

namespace fs {

namespace {

const std::vector<ModelEntry>* find_in(
    const std::vector<std::pair<std::string, std::vector<ModelEntry>>>& table,
    const std::string& provider_id) {
  for (auto& kv : table) {
    if (kv.first == provider_id) return &kv.second;
  }
  return nullptr;
}

std::vector<ModelEntry>* find_in_mut(
    std::vector<std::pair<std::string, std::vector<ModelEntry>>>& table,
    const std::string& provider_id) {
  for (auto& kv : table) {
    if (kv.first == provider_id) return &kv.second;
  }
  return nullptr;
}

}  // namespace

void ModelTable::set_preset(const std::vector<ModelEntry>& preset) { preset_ = preset; }

void ModelTable::set_api_models(const std::string& provider_id,
                                const std::vector<ModelEntry>& models) {
  auto* v = find_in_mut(api_, provider_id);
  if (v) *v = models;
  else api_.push_back({provider_id, models});
}

void ModelTable::set_user_model(const std::string& provider_id, const ModelEntry& m) {
  auto* v = find_in_mut(user_, provider_id);
  if (!v) {
    user_.push_back({provider_id, {}});
    v = &user_.back().second;
  }
  for (auto& e : *v) {
    if (e.id == m.id) { e = m; return; }
  }
  v->push_back(m);
}

bool ModelTable::remove_user_model(const std::string& provider_id,
                                   const std::string& model_id) {
  auto* v = find_in_mut(user_, provider_id);
  if (!v) return false;
  for (size_t i = 0; i < v->size(); ++i) {
    if ((*v)[i].id == model_id) {
      v->erase(v->begin() + static_cast<long>(i));
      return true;
    }
  }
  return false;
}

std::vector<ModelEntry> ModelTable::user_models(const std::string& provider_id) const {
  if (auto* v = find_in(user_, provider_id)) return *v;
  return std::vector<ModelEntry>();
}

Json ModelTable::user_models_json() const {
  Json out = Json::object();
  for (auto& kv : user_) {
    if (kv.second.empty()) continue;
    Json arr = Json::array();
    for (auto& m : kv.second) arr.push_back(m.to_json());
    out[kv.first] = arr;
  }
  return out;
}

void ModelTable::load_user_models(const Json& j) {
  user_.clear();
  if (!j.is_object()) return;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it.value().is_array()) continue;
    std::vector<ModelEntry> v;
    for (auto& e : it.value()) {
      if (!e.is_object()) continue;
      ModelEntry m = ModelEntry::from_json(e);
      if (m.id.empty()) continue;
      fill_model_defaults(&m);
      v.push_back(m);
    }
    if (!v.empty()) user_.push_back({it.key(), v});
  }
}

Json ModelTable::api_models_json() const {
  Json out = Json::object();
  for (auto& kv : api_) {
    if (kv.second.empty()) continue;
    Json arr = Json::array();
    for (auto& m : kv.second) arr.push_back(m.to_json());
    out[kv.first] = arr;
  }
  return out;
}

void ModelTable::load_api_models(const Json& j) {
  api_.clear();
  if (!j.is_object()) return;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it.value().is_array()) continue;
    std::vector<ModelEntry> v;
    for (auto& e : it.value()) {
      if (!e.is_object()) continue;
      ModelEntry m = ModelEntry::from_json(e);
      if (m.id.empty()) continue;
      m.from_api = true;
      fill_model_defaults(&m);
      v.push_back(m);
    }
    if (!v.empty()) api_.push_back({it.key(), v});
  }
}

std::vector<ModelEntry> ModelTable::list_models(const std::string& provider_id) const {
  std::vector<ModelEntry> out;
  // 用户自定义优先，并记住哪些 id 已经占位
  if (auto* u = find_in(user_, provider_id)) {
    for (auto& m : *u) out.push_back(m);
  }
  if (auto* a = find_in(api_, provider_id)) {
    for (auto& m : *a) {
      bool dup = false;
      for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].id == m.id) { dup = true; break; }
      }
      if (!dup) out.push_back(m);
    }
  }
  // 全新 provider 还没有任何模型时**不**回退预置表：一个从没连过 API 的供应商
  // 下面列出 gpt-4o、claude 之类凭空出现的条目，看起来像是已经配置好了，实际
  // 一个都调不通。空列表 + "从 API 获取 / 新增"才是真实状态。预置表仍然用于
  // resolve()：用户明确写下一个已知模型 id 时，它的能力标签是可信的。
  for (size_t i = 0; i < out.size(); ++i) fill_model_defaults(&out[i]);
  return out;
}

ModelEntry ModelTable::resolve(const std::string& provider_id,
                               const std::string& model_id) const {
  // A user entry records what the USER changed (enable/disable, a level list).
  // It must not erase facts that only the API knows - the context window above
  // all: the side panel's water level is computed from it, and a user row that
  // only flipped `enabled` used to freeze context_len at 0, which made the level
  // disappear for every model the user had ever touched.
  auto merge_missing = [this, &provider_id](ModelEntry* dst) {
    if (auto* a = find_in(api_, provider_id)) {
      for (auto& m : *a) {
        if (m.id != dst->id) continue;
        if (dst->caps.context_len <= 0) dst->caps.context_len = m.caps.context_len;
        if (dst->caps.max_output <= 0) dst->caps.max_output = m.caps.max_output;
        if (!dst->caps.vision && m.caps.vision) dst->caps.vision = true;
        if (!dst->caps.audio && m.caps.audio) dst->caps.audio = true;
        if (!dst->caps.pdf && m.caps.pdf) dst->caps.pdf = true;
        if (!dst->caps.video && m.caps.video) dst->caps.video = true;
        if (!dst->caps.structured_output && m.caps.structured_output) {
          dst->caps.structured_output = true;
        }
        if (dst->thinking.levels.empty() && !m.thinking.levels.empty()) {
          dst->thinking = m.thinking;
          dst->caps.thinking = true;
        }
        if (dst->display.empty() || dst->display == dst->id) {
          if (!m.display.empty()) dst->display = m.display;
        }
        if (dst->icon.empty()) dst->icon = m.icon;
        return;
      }
    }
    for (auto& m : preset_) {
      if (m.id != dst->id) continue;
      if (dst->caps.context_len <= 0) dst->caps.context_len = m.caps.context_len;
      if (dst->caps.max_output <= 0) dst->caps.max_output = m.caps.max_output;
      return;
    }
  };

  // 1) 用户自定义
  if (auto* u = find_in(user_, provider_id)) {
    for (auto& m : *u) {
      if (m.id == model_id) {
        ModelEntry r = m;
        merge_missing(&r);
        fill_model_defaults(&r);
        return r;
      }
    }
  }
  // 2) API 获取
  if (auto* a = find_in(api_, provider_id)) {
    for (auto& m : *a) {
      if (m.id == model_id) {
        ModelEntry r = m;
        fill_model_defaults(&r);
        return r;
      }
    }
  }
  // 3) 预置
  for (auto& m : preset_) {
    if (m.id == model_id) {
      ModelEntry r = m;
      fill_model_defaults(&r);
      return r;
    }
  }
  // 4) 保守缺省（tool_use 恒 false）
  return conservative_default(model_id);
}

ModelEntry ModelTable::conservative_default(const std::string& model_id) {
  ModelEntry m;
  m.id = model_id;
  m.display = model_id;
  m.caps.tool_use = false;  // 不支持 toolUse → 拒绝进会话
  m.caps.vision = false;
  m.caps.thinking = false;
  m.caps.context_len = 0;
  m.from_preset = false;
  m.from_api = false;
  return m;
}

// 一键获取：走 NetClient 调 /models 端点；失败回退上一级并提示
// v1：OpenAI Chat/custom 形态调 {base}/models；其他形态直接回退
ModelTable::FetchResult ModelTable::fetch_models(const std::string& provider_id,
                                                 const ProviderConn& conn,
                                                 const NetClient& net) const {
  FetchResult r;
  if (conn.kind != ApiKind::kOpenAIChat && conn.kind != ApiKind::kCustom) {
    // Response/Anthropic/Plan：v1 不实现 /models，直接回退
    r.fallback_level = "user";
    r.message = "this api kind does not support list models, fallback";
    return r;
  }
  HttpRequest req;
  req.method = "GET";
  req.url = conn.base_url + "/models";
  req.headers["Authorization"] = "Bearer " + conn.api_key;
  req.timeout_ms = 15000;
  HttpResponse resp;
  FsResult nr = net.request(req, &resp);
  if (!nr.ok() || resp.status != 200) {
    r.fallback_level = "user";
    r.message = nr.ok() ? ("models fetch http " + std::to_string(resp.status))
                        : nr.message;
    return r;
  }
  try {
    Json j = Json::parse(resp.body);
    Json data = j.contains("data") && j["data"].is_array() ? j["data"] : Json::array();
    std::vector<ModelEntry> models;
    for (auto& e : data) {
      if (!e.is_object()) continue;
      std::string id = jsonutil::get<std::string>(e, "id", "");
      if (id.empty()) continue;
      ModelEntry m;
      m.id = id;
      m.display = id;
      m.from_api = true;
      // Context window when the endpoint reports it (OpenRouter: context_length;
      // others: context_window / max_context_length / max_model_len, sometimes
      // nested under top_provider). The UI's context water level is computed
      // from THIS value, so it must not be guessed from the model name when the
      // API actually tells us.
      const char* kCtxKeys[] = {"context_length", "context_window",
                                "max_context_length", "max_model_len"};
      for (size_t k = 0; k < sizeof(kCtxKeys) / sizeof(kCtxKeys[0]); ++k) {
        int64_t n = jsonutil::get_int(e, kCtxKeys[k], 0);
        if (n <= 0 && e.contains("top_provider") && e["top_provider"].is_object()) {
          n = jsonutil::get_int(e["top_provider"], kCtxKeys[k], 0);
        }
        if (n > 0) {
          m.caps.context_len = n;
          break;
        }
      }
      // API 不一定给能力标签：保守置 tool_use=true（[OI] chat 形态默认支持工具）；
      // 用户可在设置里手动改（provider.md §4 允许覆盖）
      m.caps.tool_use = true;

      // ---- fill every fact the endpoint DOES report -------------------------
      // One fetch should be enough to configure a model. Anything the endpoint
      // states here is authoritative; the UI then needs no manual entry and no
      // guess from the model name.
      std::string display = jsonutil::get<std::string>(e, "name", "");
      if (!display.empty()) m.display = display;
      int64_t max_out = jsonutil::get_int(e, "max_output_tokens", 0);
      if (max_out <= 0) max_out = jsonutil::get_int(e, "max_completion_tokens", 0);
      if (max_out > 0) m.caps.max_output = max_out;
      // Modalities: "input_modalities": ["text","image"] (OpenRouter-style) or
      // the older "architecture":{"input_modalities":[...]} nesting.
      auto has_modality = [&e](const char* what) {
        const Json* arr = nullptr;
        if (e.contains("input_modalities") && e["input_modalities"].is_array()) {
          arr = &e["input_modalities"];
        } else if (e.contains("architecture") && e["architecture"].is_object() &&
                   e["architecture"].contains("input_modalities") &&
                   e["architecture"]["input_modalities"].is_array()) {
          arr = &e["architecture"]["input_modalities"];
        }
        if (!arr) return false;
        for (size_t i = 0; i < arr->size(); ++i) {
          if ((*arr)[i].is_string() && (*arr)[i].get<std::string>() == what) {
            return true;
          }
        }
        return false;
      };
      if (has_modality("image")) m.caps.vision = true;
      if (has_modality("audio")) m.caps.audio = true;
      if (has_modality("video")) m.caps.video = true;
      if (has_modality("pdf") || has_modality("file")) m.caps.pdf = true;
      // Reasoning levels: {"effort":{"supported_levels":[...],
      // "default_level":"high"}} (DeepSeek/opencode-style). This is the model's
      // OWN ladder, so it beats the name-based guess in fill_model_defaults.
      if (e.contains("effort") && e["effort"].is_object()) {
        const Json& ef = e["effort"];
        if (ef.contains("supported_levels") && ef["supported_levels"].is_array()) {
          for (size_t i = 0; i < ef["supported_levels"].size(); ++i) {
            if (ef["supported_levels"][i].is_string()) {
              m.thinking.levels.push_back(
                  ef["supported_levels"][i].get<std::string>());
            }
          }
        }
        m.thinking.default_level = jsonutil::get<std::string>(ef, "default_level", "");
        if (!m.thinking.levels.empty()) {
          m.caps.thinking = true;
          if (m.thinking.default_level.empty()) {
            m.thinking.default_level = m.thinking.levels[0];
          }
        }
      }
      // Structured output / tool use flags, when the endpoint spells them out.
      if (e.contains("capabilities") && e["capabilities"].is_object()) {
        const Json& c = e["capabilities"];
        if (c.contains("tools") || c.contains("tool_use")) {
          m.caps.tool_use = jsonutil::get<bool>(
              c, c.contains("tools") ? "tools" : "tool_use", true);
        }
        if (c.contains("structured_output")) {
          m.caps.structured_output =
              jsonutil::get<bool>(c, "structured_output", false);
        }
        if (c.contains("vision")) {
          m.caps.vision = jsonutil::get<bool>(c, "vision", m.caps.vision);
        }
      }
      models.push_back(m);
    }
    // Fill what the endpoint did NOT report (context window, thinking ladder,
    // icon). One fetch should configure the model; leaving context at 0 made the
    // UI require manual entry ("获取列表后上下文没有自动填上").
    for (size_t i = 0; i < models.size(); ++i) fill_model_defaults(&models[i]);
    if (models.empty()) {
      r.fallback_level = "user";
      r.message = "models fetch returned empty list, fallback";
      return r;
    }
    r.ok = true;
    r.models = models;
    r.fallback_level = "api";
    r.message = "";
    return r;
  } catch (...) {
    r.fallback_level = "user";
    r.message = "models fetch parse failed, fallback";
    return r;
  }
}

std::vector<ModelEntry> builtin_models_preset() {
  // 预置常见模型 + 能力标签（保守：tool_use 仅知名支持者置 true）
  std::vector<ModelEntry> v;
  auto add = [&v](const std::string& id, bool vision, bool thinking, bool tool_use,
                  int64_t ctx) {
    ModelEntry m;
    m.id = id;
    m.display = id;
    m.caps.vision = vision;
    m.caps.thinking = thinking;
    m.caps.tool_use = tool_use;
    m.caps.context_len = ctx;
    m.from_preset = true;
    fill_model_defaults(&m);
    v.push_back(m);
  };
  add("gpt-4o", true, false, true, 128000);
  add("gpt-4o-mini", true, false, true, 128000);
  add("gpt-4.1", true, false, true, 1000000);
  add("gpt-4.1-mini", true, false, true, 1000000);
  add("o4-mini", true, true, true, 200000);
  add("deepseek-chat", false, false, true, 128000);
  add("deepseek-reasoner", false, true, true, 128000);
  add("gemini-2.5-pro", true, true, true, 1048576);
  add("gemini-2.5-flash", true, true, true, 1048576);
  return v;
}

// Best-effort context window for a model id. Only used when the endpoint does
// NOT report one (the common case for OpenAI-compatible /models): without it the
// UI left the field empty and the context water level could not be computed.
//
// The default is deliberately GENEROUS: by now the mainstream cloud models all
// ship a 1M window, so the old family table (claude 200k, deepseek 128k, ...)
// understated every one of them and a too-small fallback shrinks the usable
// workspace for no reason. Only explicit LOW-TIER markers in the name ("mini",
// "nano", "flash", "lite", size suffixes like 7b) pull it back down.
int64_t default_context_for(const std::string& model_id) {
  std::string s;
  s.reserve(model_id.size());
  for (size_t i = 0; i < model_id.size(); ++i) {
    char c = model_id[i];
    s.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  // Gemini's Flash line is still a 1M model, so the family wins over the word.
  if (s.find("gemini") != std::string::npos) return 1048576;
  // Split into name TOKENS instead of raw substring matching: "minimax" must not
  // be read as "mini", and a size suffix only counts when it is its own token.
  std::vector<std::string> tok;
  std::string cur;
  for (size_t i = 0; i <= s.size(); ++i) {
    char c = i < s.size() ? s[i] : '\0';
    bool sep = (c == '\0' || c == '-' || c == '_' || c == '.' || c == '/' ||
                c == ':' || c == ' ' || c == '@' || c == '+');
    if (sep) {
      if (!cur.empty()) {
        tok.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  static const char* const kCompact[] = {
      "mini", "nano", "lite", "small", "tiny", "flash",
      "turbo", "haiku", "air",           nullptr};
  for (size_t t = 0; t < tok.size(); ++t) {
    for (size_t k = 0; kCompact[k]; ++k) {
      if (tok[t] == kCompact[k]) return 131072;
    }
    // 7b / 8b / 1.5b / 0.5b ...: a bare parameter count says "small model".
    const std::string& w = tok[t];
    bool size = w.size() >= 2 && w[w.size() - 1] == 'b';
    for (size_t i = 0; size && i + 1 < w.size(); ++i) {
      char c = w[i];
      bool num = (c >= '0' && c <= '9') || c == '.';
      if (!num && c != 'x') size = false;
    }
    if (size) return 131072;
  }
  return 1048576;
}

// 补齐默认值：图标与思考档位由模型名粗匹配得出（实现见 model_naming.cpp）。
void fill_model_defaults(ModelEntry* m) {
  if (!m) return;
  const std::string derived_icon = model_icon_for(m->id);
  // A persisted API/preset icon was computed by an OLDER name matcher: e.g.
  // "muse_spark_..." was stored as iFlytek's "spark" before the Meta rule
  // existed. Those rows are not user choices, so recompute them; a user-saved
  // icon (user_set) is never touched. Without this the wrong brand survived in
  // config forever, because fill only looked at an EMPTY icon.
  if (!derived_icon.empty() && !m->user_set && (m->from_api || m->from_preset)) {
    m->icon = derived_icon;
  } else if (m->icon.empty()) {
    m->icon = derived_icon;
  }
  // Context length: endpoints rarely report it on /models. A model with no
  // context window made the water level uncomputable, so fall back to the
  // known preset or the model family's documented window - never leave it 0.
  if (m->caps.context_len <= 0) m->caps.context_len = default_context_for(m->id);
  // A model that carries a DEFAULT thinking level but no ladder (the preset /
  // user data stores the two separately) is the common case: the composer then
  // offered only 开/关 until the settings page was opened and saved, because the
  // save path filled the ladder while this one required BOTH fields to be empty.
  if (m->thinking.levels.empty()) {
    m->thinking.levels = thinking_levels_for(m->id);
    if (!m->thinking.levels.empty()) {
      m->caps.thinking = true;
      bool ok = false;
      for (size_t i = 0; i < m->thinking.levels.size(); ++i) {
        if (m->thinking.levels[i] == m->thinking.default_level) ok = true;
      }
      if (!ok) m->thinking.default_level = m->thinking.levels[0];
    }
  }
}

}  // namespace fs
