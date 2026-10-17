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
  // 1) 用户自定义
  if (auto* u = find_in(user_, provider_id)) {
    for (auto& m : *u) {
      if (m.id == model_id) {
        ModelEntry r = m;
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
      // API 不一定给能力标签：保守置 tool_use=true（OpenAI chat 形态默认支持工具）；
      // 用户可在设置里手动改（provider.md §4 允许覆盖）
      m.caps.tool_use = true;
      models.push_back(m);
    }
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
  add("", true, true, true, 200000);
  add("", true, false, true, 200000);
  add("", true, false, true, 200000);
  return v;
}

// 补齐默认值：图标与思考档位由模型名粗匹配得出（实现见 model_naming.cpp）。
void fill_model_defaults(ModelEntry* m) {
  if (!m) return;
  if (m->icon.empty()) m->icon = model_icon_for(m->id);
  // Only fill the thinking config when there is none. A model that already
  // carries levels (or a deliberate empty one) has been set by the user, and
  // the capability flag is theirs too: caps.thinking and the level list are not
  // the same thing (a model can reason without offering a ladder), so deriving
  // one from the other would overwrite a considered answer with a guess.
  if (m->thinking.levels.empty() && m->thinking.default_level.empty()) {
    m->thinking.levels = thinking_levels_for(m->id);
    m->thinking.default_level = default_thinking_for(m->id);
    if (!m->thinking.levels.empty()) m->caps.thinking = true;
  }
}

}  // namespace fs
