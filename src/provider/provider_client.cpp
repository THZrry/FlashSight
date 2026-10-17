// provider_client.cpp - 按形态构造请求 + 解析响应（网络走 NetClient，可桩）
#include "provider_client.hpp"

#include <chrono>

namespace fs {

std::string mask_secret(const std::string& s) {
  if (s.size() <= 8) return std::string(s.size(), '*');
  return s.substr(0, 4) + "****" + s.substr(s.size() - 4);
}

ProviderClient::ProviderClient(const ProviderConn& conn, const NetClient* net)
    : conn_(conn), net_(net) {}

FsResult ProviderClient::chat(const ChatRequest& req, ChatResponse* out,
                              const StreamDeltaFn& on_delta) const {
  if (!out) return FsResult::error(ErrCode::kInvalidArg, "null response");
  auto t0 = std::chrono::steady_clock::now();
  FsResult r;
  switch (conn_.kind) {
    case ApiKind::kOpenAIChat:
    case ApiKind::kCustom:
    case ApiKind::kPlan:  // legacy value: same chat shape, session header set
      r = chat_openai_chat(req, out, on_delta);
      break;
    case ApiKind::kOpenAIResponse:
      r = chat_openai_response(req, out, on_delta);
      break;
    case ApiKind::kAnthropic:
      r = chat_anthropic(req, out, on_delta);
      break;
    default:
      r = FsResult::error(ErrCode::kNotSupported, "unknown api kind");
      break;
  }
  out->latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
  return r;
}

// ----- OpenAI Chat Completions -----
// 自定义头的值里可以写 {{SESSION}}，替换成当前会话 id。这样 opencode 系的
// 会话亲和头和 Azure 的 api-key 走同一条路径：都是"模板预填的一条普通头"，
// 没有"某种供应商才有的隐藏开关"。
namespace {
std::string subst_session(const std::string& v, const std::string& sid) {
  std::string out = v;
  const std::string k = "{{SESSION}}";
  size_t p = 0;
  while ((p = out.find(k, p)) != std::string::npos) {
    out.replace(p, k.size(), sid);
    p += sid.size();
  }
  return out;
}
}  // namespace

FsResult ProviderClient::chat_openai_chat(const ChatRequest& req, ChatResponse* out,
                                          const StreamDeltaFn& on_delta) const {
  Json body = Json::object();
  body["model"] = req.model;
  body["messages"] = req.messages;
  if (!req.tools.is_null() && !req.tools.empty()) body["tools"] = req.tools;
  body["temperature"] = req.temperature;
  body["max_tokens"] = req.max_tokens;
  body["stream"] = req.stream;
  // 供应商特有的请求体字段（浅合并；不覆盖上面构造好的标准字段，除非用户
  // 明确写了同名字段——那正是"自定义 body"的意义）
  if (conn_.body_override.is_object()) {
    for (auto it = conn_.body_override.begin(); it != conn_.body_override.end(); ++it) {
      body[it.key()] = it.value();
    }
  }

  HttpRequest http;
  http.method = "POST";
  http.url = conn_.base_url + "/chat/completions";
  http.headers["Content-Type"] = "application/json";
  http.headers["Authorization"] = "Bearer " + conn_.api_key;
  // 会话亲和头：这是供应商的一个开关（opencode 系端点需要），不是请求形态
  if (conn_.send_session) {
    if (!req.session_id.empty()) {
      http.headers["x-opencode-session"] = req.session_id;
      http.headers["x-opencode-session-id"] = req.session_id;
    }
    http.headers["x-opencode-client"] = "flashsight";
  }
  for (auto& kv : conn_.custom_headers) {
    http.headers[kv.first] = subst_session(kv.second, req.session_id);
  }
  http.body = body.dump();
  http.timeout_ms = 120000;

  HttpResponse resp;
  FsResult nr = net_->request(http, &resp);
  if (!nr.ok()) return nr;
  out->http_status = resp.status;
  if (resp.status == 402) {
    out->error = "quota exhausted (402 balance-zero)";
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  if (resp.status == 429) {
    out->error = "rate limited (429)";
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  if (resp.status != 200) {
    out->error = "http " + std::to_string(resp.status);
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  try {
    Json j = Json::parse(resp.body);
    // text 累积
    Json choices = j.contains("choices") && j["choices"].is_array()
                       ? j["choices"] : Json::array();
    if (!choices.empty()) {
      Json msg = choices[0].contains("message") ? choices[0]["message"] : Json::object();
      out->text = jsonutil::get<std::string>(msg, "content", "");
      // tool_calls（P3 解析执行）
      if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        out->tool_calls = msg["tool_calls"];
      }
    }
    // usage
    Json u = j.contains("usage") && j["usage"].is_object() ? j["usage"] : Json::object();
    out->in_tokens = jsonutil::get_int(u, "prompt_tokens", 0);
    out->out_tokens = jsonutil::get_int(u, "completion_tokens", 0);
    out->cache_tokens = jsonutil::get_int(u, "prompt_tokens_cached", 0);
    // cost（opencode 实证：Zen 注入 .cost；"0"=配额内，美元字符串=余额扣费）
    // 成本三态：cost=="0" -> quota；可解析金额 -> money；否则连接层 quota/unknown
    std::string cost_str = jsonutil::get<std::string>(j, "cost", "");
    if (!cost_str.empty()) {
      if (cost_str == "0" || cost_str == "0.0" || cost_str == "0.00000000") {
        out->cost_kind = "quota";
        out->cost = 0.0;
      } else {
        try {
          out->cost = std::stod(cost_str);
          out->cost_kind = "money";
        } catch (...) {
          out->cost_kind = conn_.has_quota ? conn_.quota_kind : "unknown";
          out->cost = conn_.has_quota ? conn_.quota_value : 0.0;
        }
      }
    } else {
      out->cost_kind = conn_.has_quota ? conn_.quota_kind : "unknown";
      out->cost = conn_.has_quota ? conn_.quota_value : 0.0;
    }
    // 流式：body 已是完整 JSON（NetClient v1 不支持分块流；P2 验收走完整响应 + on_delta 全量推）
    if (req.stream && on_delta && !out->text.empty()) on_delta(out->text);
    return FsResult::success();
  } catch (...) {
    out->error = "response parse failed";
    return FsResult::error(ErrCode::kParse, out->error);
  }
}

// ----- OpenAI Response API -----
FsResult ProviderClient::chat_openai_response(const ChatRequest& req, ChatResponse* out,
                                              const StreamDeltaFn& on_delta) const {
  Json body = Json::object();
  body["model"] = req.model;
  body["input"] = req.messages;  // Response API 用 input 数组
  if (!req.tools.is_null() && !req.tools.empty()) body["tools"] = req.tools;
  body["max_output_tokens"] = req.max_tokens;
  body["stream"] = req.stream;

  HttpRequest http;
  http.method = "POST";
  http.url = conn_.base_url + "/responses";
  http.headers["Content-Type"] = "application/json";
  http.headers["Authorization"] = "Bearer " + conn_.api_key;
  for (auto& kv : conn_.custom_headers) {
    http.headers[kv.first] = subst_session(kv.second, req.session_id);
  }
  http.body = body.dump();
  http.timeout_ms = 120000;

  HttpResponse resp;
  FsResult nr = net_->request(http, &resp);
  if (!nr.ok()) return nr;
  out->http_status = resp.status;
  if (resp.status != 200) {
    out->error = "http " + std::to_string(resp.status);
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  try {
    Json j = Json::parse(resp.body);
    // Response API 输出在 output[].content[].text
    Json output = j.contains("output") && j["output"].is_array()
                      ? j["output"] : Json::array();
    std::string text;
    for (auto& item : output) {
      if (!item.is_object()) continue;
      Json content = item.contains("content") && item["content"].is_array()
                         ? item["content"] : Json::array();
      for (auto& c : content) {
        if (c.is_object()) {
          std::string t = jsonutil::get<std::string>(c, "text", "");
          if (!t.empty()) text += t;
        }
      }
    }
    out->text = text;
    Json u = j.contains("usage") && j["usage"].is_object() ? j["usage"] : Json::object();
    out->in_tokens = jsonutil::get_int(u, "input_tokens", 0);
    out->out_tokens = jsonutil::get_int(u, "output_tokens", 0);
    out->cache_tokens = jsonutil::get_int(u, "cached_input_tokens", 0);
    out->cost_kind = conn_.has_quota ? conn_.quota_kind : "unknown";
    out->cost = conn_.has_quota ? conn_.quota_value : 0.0;
    if (req.stream && on_delta && !out->text.empty()) on_delta(out->text);
    return FsResult::success();
  } catch (...) {
    out->error = "response parse failed";
    return FsResult::error(ErrCode::kParse, out->error);
  }
}

// ----- Anthropic Messages -----
FsResult ProviderClient::chat_anthropic(const ChatRequest& req, ChatResponse* out,
                                        const StreamDeltaFn& on_delta) const {
  Json body = Json::object();
  body["model"] = req.model;
  body["messages"] = req.messages;  // role/user/assistant；system 单独字段
  body["max_tokens"] = req.max_tokens;
  if (!req.tools.is_null() && !req.tools.empty()) body["tools"] = req.tools;
  body["stream"] = req.stream;

  HttpRequest http;
  http.method = "POST";
  http.url = conn_.base_url + "/messages";
  http.headers["Content-Type"] = "application/json";
  http.headers["x-api-key"] = conn_.api_key;
  http.headers["anthropic-version"] = "2023-06-01";
  for (auto& kv : conn_.custom_headers) {
    http.headers[kv.first] = subst_session(kv.second, req.session_id);
  }
  http.body = body.dump();
  http.timeout_ms = 120000;

  HttpResponse resp;
  FsResult nr = net_->request(http, &resp);
  if (!nr.ok()) return nr;
  out->http_status = resp.status;
  if (resp.status != 200) {
    out->error = "http " + std::to_string(resp.status);
    return FsResult::error(ErrCode::kRemote, out->error);
  }
  try {
    Json j = Json::parse(resp.body);
    Json content = j.contains("content") && j["content"].is_array()
                       ? j["content"] : Json::array();
    std::string text;
    for (auto& c : content) {
      if (c.is_object() && jsonutil::get<std::string>(c, "type", "") == "text") {
        text += jsonutil::get<std::string>(c, "text", "");
      }
    }
    out->text = text;
    Json u = j.contains("usage") && j["usage"].is_object() ? j["usage"] : Json::object();
    out->in_tokens = jsonutil::get_int(u, "input_tokens", 0);
    out->out_tokens = jsonutil::get_int(u, "output_tokens", 0);
    out->cache_tokens = jsonutil::get_int(u, "cache_read_input_tokens", 0);
    out->cost_kind = conn_.has_quota ? conn_.quota_kind : "unknown";
    out->cost = conn_.has_quota ? conn_.quota_value : 0.0;
    if (req.stream && on_delta && !out->text.empty()) on_delta(out->text);
    return FsResult::success();
  } catch (...) {
    out->error = "response parse failed";
    return FsResult::error(ErrCode::kParse, out->error);
  }
}

// ----- testConnection：轻量探活（发一条最小 chat 或 /models）-----
FsResult ProviderClient::test_connection(ChatResponse* out) const {
  ChatRequest req;
  req.model = "gpt-4o-mini";  // 探活用最小模型；实际由调用方覆盖
  Json msg = Json::object();
  msg["role"] = "user";
  msg["content"] = "ping";
  req.messages = Json::array({msg});
  req.max_tokens = 8;
  req.stream = false;
  return chat(req, out, nullptr);
}

void ProviderClient::parse_openai_chat_response(const Json& body, ChatResponse* out) {
  Json choices = body.contains("choices") && body["choices"].is_array()
                     ? body["choices"] : Json::array();
  if (!choices.empty()) {
    Json msg = choices[0].contains("message") ? choices[0]["message"] : Json::object();
    out->text = jsonutil::get<std::string>(msg, "content", "");
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      out->tool_calls = msg["tool_calls"];
    }
  }
}

void ProviderClient::parse_anthropic_response(const Json& body, ChatResponse* out) {
  Json content = body.contains("content") && body["content"].is_array()
                     ? body["content"] : Json::array();
  std::string text;
  for (auto& c : content) {
    if (c.is_object() && jsonutil::get<std::string>(c, "type", "") == "text") {
      text += jsonutil::get<std::string>(c, "text", "");
    }
  }
  out->text = text;
}

}  // namespace fs