// plan_manager.cpp - coding-plan OAuth connections + quota probes.
#include "plan_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include <httplib.h>
#include <mbedtls/sha256.h>

#include "fs_log.hpp"

namespace fs {

namespace {

struct PlanProvider {
  const char* id;
  const char* name;
  const char* auth;       // "oauth" | "poll" | "apikey"
  const char* client_id;
  const char* client_secret;
  const char* auth_url;
  const char* token_url;
  const char* scope;
  int callback_port;
  const char* callback_path;
  const char* base_url;
  const char* kind;
  const char* key_url;
  int64_t models_mask;
};

// The catalog is deliberately explicit: every provider has a different flow.
// Claude/Codex/Gemini are authorization-code (+PKCE); GLM is ZCode's polling
// flow; the three CN plans use subscription API keys on their own gateways.
const PlanProvider kPlans[] = {
    {"claude", "Claude Code (Pro/Max)", "oauth",
     "9d1c250a-e61b-44d9-88ed-5944d1962f5e", "",
     "https://claude.ai/oauth/authorize",
     "https://api.anthropic.com/v1/oauth/token",
     "org:create_api_key user:profile user:inference", 54545, "/callback",
     "https://api.anthropic.com/v1", "anthropic",
     "https://claude.ai", 0},
    {"codex", "OpenAI Codex (ChatGPT Plus/Pro)", "oauth",
     "app_EMoamEEZ73f0CkXaXp7hrann", "",
     "https://auth.openai.com/oauth/authorize",
     "https://auth.openai.com/oauth/token",
     "openid profile email offline_access", 1455, "/auth/callback",
     "https://chatgpt.com/backend-api/codex", "responses",
     "https://chatgpt.com/codex", 0},
    {"gemini", "Gemini Code Assist", "oauth",
     "681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com",
     "",
     "https://accounts.google.com/o/oauth2/v2/auth",
     "https://oauth2.googleapis.com/token",
     "https://www.googleapis.com/auth/cloud-platform "
     "https://www.googleapis.com/auth/userinfo.email "
     "https://www.googleapis.com/auth/userinfo.profile",
     8085, "/callback",
     "https://generativelanguage.googleapis.com/v1beta/openai", "openai",
     "https://github.com/google-gemini/gemini-cli", 0},
    {"glm", "Z.ai GLM Coding Plan", "poll", "", "", "", "", "", 0, "",
     "https://api.z.ai/api/coding/paas/v4", "openai",
     "https://z.ai", 0},
    {"volc", "火山方舟 Coding Plan", "apikey", "", "", "", "", "", 0, "",
     "https://ark.cn-beijing.volces.com/api/coding/v3", "openai",
     "https://console.volcengine.com/ark/region:ark+cn-beijing/apiKey", 0},
    {"qwen", "Alibaba Bailian Coding Plan", "apikey", "", "", "", "", "", 0, "",
     "https://token-plan.maas.qwencloudapi.com/compatible-mode/v1", "openai",
     "https://home.qwencloud.com/api-keys", 0},
    {"tencent", "腾讯混元 Coding Plan", "apikey", "", "", "", "", "", 0, "",
     "https://api.hunyuan.cloud.tencent.com/v1", "openai",
     "https://console.cloud.tencent.com/hunyuan/api-key", 0},
};

const PlanProvider* find_plan(const std::string& id) {
  for (size_t i = 0; i < sizeof(kPlans) / sizeof(kPlans[0]); ++i) {
    if (id == kPlans[i].id) return &kPlans[i];
  }
  return nullptr;
}

std::string b64url(const unsigned char* data, size_t len) {
  static const char* tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    unsigned v = static_cast<unsigned>(data[i]) << 16;
    if (i + 1 < len) v |= static_cast<unsigned>(data[i + 1]) << 8;
    if (i + 2 < len) v |= data[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    if (i + 1 < len) out += tbl[(v >> 6) & 63];
    if (i + 2 < len) out += tbl[v & 63];
  }
  return out;
}

std::string random_hex(size_t bytes) {
  std::random_device rd;
  std::string out;
  static const char* hex = "0123456789abcdef";
  for (size_t i = 0; i < bytes; ++i) {
    unsigned v = rd();
    out += hex[v & 15];
    out += hex[(v >> 4) & 15];
  }
  return out;
}

std::string pkce_challenge(const std::string& verifier) {
  unsigned char digest[32];
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(verifier.data()),
                     verifier.size(), digest, 0) != 0) {
    return std::string();
  }
  return b64url(digest, sizeof(digest));
}

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string form_body(const std::map<std::string, std::string>& kv) {
  std::string out;
  for (std::map<std::string, std::string>::const_iterator it = kv.begin();
       it != kv.end(); ++it) {
    if (!out.empty()) out += '&';
    out += url_encode(it->first) + "=" + url_encode(it->second);
  }
  return out;
}

std::string lower(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
  }
  return s;
}

int64_t now_ms_clock(const Clock* c) {
  return c ? c->now_ms() : WallClock().now_ms();
}

// Google's installed-app client secret is not a real security boundary, but it
// is still a credential and must not live in the repository (GitHub secret
// scanning blocks it). Supply it at runtime instead:
//   FLASHSIGHT_GOOGLE_CLIENT_SECRET=...
std::string google_client_secret() {
  const char* v = std::getenv("FLASHSIGHT_GOOGLE_CLIENT_SECRET");
  return (v && *v) ? std::string(v) : std::string();
}

// Quota reset times arrive as an epoch (Codex/GLM) or ISO-8601 (Claude/Gemini).
int64_t reset_ms(const Json& q) {
  if (!q.is_object() || !q.contains("resetAt")) return 0;
  const Json& v = q["resetAt"];
  if (v.is_number()) return v.get<int64_t>();
  if (!v.is_string()) return 0;
  std::string s = v.get<std::string>();
  if (s.empty()) return 0;
  struct tm t;
  std::memset(&t, 0, sizeof(t));
  std::istringstream in(s);
  in >> std::get_time(&t, "%Y-%m-%dT%H:%M");
  if (in.fail()) return 0;
#ifdef _WIN32
  time_t epoch = _mkgmtime(&t);
#else
  time_t epoch = timegm(&t);
#endif
  return epoch > 0 ? static_cast<int64_t>(epoch) * 1000 : 0;
}

// Decode a JWT payload (no signature check - it is the token we just received
// over TLS) and pull the ChatGPT account id out.
std::string jwt_account_id(const std::string& jwt) {
  size_t a = jwt.find('.');
  if (a == std::string::npos) return "";
  size_t b = jwt.find('.', a + 1);
  if (b == std::string::npos) return "";
  std::string payload = jwt.substr(a + 1, b - a - 1);
  for (size_t i = 0; i < payload.size(); ++i) {
    if (payload[i] == '-') payload[i] = '+';
    else if (payload[i] == '_') payload[i] = '/';
  }
  while (payload.size() % 4) payload += '=';
  // base64 decode
  std::string bin;
  int val = 0, bits = -8;
  for (size_t i = 0; i < payload.size(); ++i) {
    char c = payload[i];
    if (c == '=') break;
    int d = 0;
    if (c >= 'A' && c <= 'Z') d = c - 'A';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
    else if (c >= '0' && c <= '9') d = c - '0' + 52;
    else if (c == '+') d = 62;
    else if (c == '/') d = 63;
    else continue;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      bin += static_cast<char>((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  try {
    Json j = Json::parse(bin);
    Json auth = j.value("https://api.openai.com/auth", Json::object());
    std::string id = jsonutil::get<std::string>(auth, "chatgpt_account_id", "");
    if (id.empty()) id = jsonutil::get<std::string>(j, "chatgpt_account_id", "");
    return id;
  } catch (...) {
    return "";
  }
}

}  // namespace

struct PlanManager::Flow {
  std::string id;
  const PlanProvider* def = nullptr;
  std::string verifier;
  std::string state;
  std::string redirect_uri;
  std::string authorize_url;
  std::string code;
  std::string callback_state;
  std::string poll_token;
  std::string flow_id;
  std::string error;
  std::string status = "pending";  // pending | ready | error | cancelled
  int64_t created_ms = 0;
  int64_t expires_ms = 0;
  std::shared_ptr<httplib::Server> srv;
  std::thread server_thread;
  std::atomic<bool> server_stop{false};

  ~Flow() {
    server_stop.store(true);
    if (srv) srv->stop();
    if (server_thread.joinable()) server_thread.join();
  }
};

struct PlanManager::Conn {
  std::string id;
  std::string provider;
  std::string label;
  std::string auth_type;  // oauth | apikey
  std::string access;
  std::string refresh;
  std::string account_id;
  std::string plan;
  std::string region;
  std::string meta;
  int64_t expires_at = 0;
  int64_t created_at = 0;
  int64_t last_usage_at = 0;
  Json usage = Json::object();
  Json to_json() const {
    Json j = Json::object();
    j["id"] = id;
    j["provider"] = provider;
    j["label"] = label;
    j["authType"] = auth_type;
    j["accessToken"] = access;
    j["refreshToken"] = refresh;
    j["accountId"] = account_id;
    j["plan"] = plan;
    j["region"] = region;
    j["meta"] = meta;
    j["expiresAt"] = expires_at;
    j["createdAt"] = created_at;
    j["lastUsageAt"] = last_usage_at;
    j["usage"] = usage;
    return j;
  }
};

PlanManager::PlanManager(const ghc::filesystem::path& root, NetClient* net,
                         const Clock* clock)
    : file_(root / "plans.json"), net_(net), clock_(clock) {
  load();
}

PlanManager::~PlanManager() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  flows_.clear();
}

FsResult PlanManager::load() {
  doc_ = Json::object();
  if (!ghc::filesystem::exists(file_)) return FsResult::success();
  std::ifstream in(file_.string(), std::ios::binary);
  if (!in) return FsResult::success();
  std::stringstream ss;
  ss << in.rdbuf();
  try {
    Json j = Json::parse(ss.str());
    if (j.is_object()) doc_ = j;
  } catch (...) {
  }
  if (!doc_.contains("connections") || !doc_["connections"].is_array()) {
    doc_["connections"] = Json::array();
  }
  return FsResult::success();
}

FsResult PlanManager::save() {
  std::string err;
  if (!jsonutil::write_atomic(file_.string(), doc_, &err)) {
    return FsResult::error(ErrCode::kIo, err);
  }
  return FsResult::success();
}

Json PlanManager::conn_private(const Conn& c) const { return c.to_json(); }

Json PlanManager::conn_public(const Conn& c) const {
  Json j = c.to_json();
  j.erase("accessToken");
  j.erase("refreshToken");
  j["connected"] = !c.access.empty();
  return j;
}

Json PlanManager::list() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  Json providers = Json::array();
  for (size_t i = 0; i < sizeof(kPlans) / sizeof(kPlans[0]); ++i) {
    Json p = Json::object();
    p["id"] = kPlans[i].id;
    p["name"] = kPlans[i].name;
    p["auth"] = kPlans[i].auth;
    p["keyUrl"] = kPlans[i].key_url;
    providers.push_back(p);
  }
  Json conns = Json::array();
  const Json& arr = doc_.contains("connections") && doc_["connections"].is_array()
                        ? doc_["connections"]
                        : Json::array();
  for (size_t i = 0; i < arr.size(); ++i) {
    Conn c;
    c.id = jsonutil::get<std::string>(arr[i], "id", "");
    c.provider = jsonutil::get<std::string>(arr[i], "provider", "");
    c.label = jsonutil::get<std::string>(arr[i], "label", "");
    c.auth_type = jsonutil::get<std::string>(arr[i], "authType", "");
    c.access = jsonutil::get<std::string>(arr[i], "accessToken", "");
    c.refresh = jsonutil::get<std::string>(arr[i], "refreshToken", "");
    c.account_id = jsonutil::get<std::string>(arr[i], "accountId", "");
    c.plan = jsonutil::get<std::string>(arr[i], "plan", "");
    c.region = jsonutil::get<std::string>(arr[i], "region", "");
    c.meta = jsonutil::get<std::string>(arr[i], "meta", "");
    c.expires_at = jsonutil::get_int(arr[i], "expiresAt", 0);
    c.created_at = jsonutil::get_int(arr[i], "createdAt", 0);
    c.last_usage_at = jsonutil::get_int(arr[i], "lastUsageAt", 0);
    c.usage = arr[i].contains("usage") ? arr[i]["usage"] : Json::object();
    conns.push_back(conn_public(c));
  }
  return Json{{"providers", providers}, {"connections", conns}};
}

FsResult PlanManager::start(const std::string& provider, bool remote) {
  const PlanProvider* def = find_plan(provider);
  if (!def) return FsResult::error(ErrCode::kInvalidArg, "unknown plan provider");
  if (remote) {
    return FsResult::error(
        ErrCode::kNotSupported,
        "coding-plan login needs a browser redirect on this machine; open the app locally (remote access is disabled for plan login)");
  }
  if (std::string(def->auth) == "apikey") {
    return FsResult::error(ErrCode::kInvalidArg,
                           "this plan uses an API key; use plan.addKey");
  }
  std::shared_ptr<Flow> f(new Flow());
  f->def = def;
  f->id = "fl_" + random_hex(8);
  f->created_ms = now_ms_clock(clock_);
  f->expires_ms = f->created_ms + 10 * 60 * 1000;
  if (std::string(def->auth) == "poll") {
    // ZCode CLI polling: init mints a poll token + a server-generated URL.
    std::string poll_token = random_hex(32);
    HttpRequest req;
    req.method = "POST";
    req.url = "https://zcode.z.ai/api/v1/oauth/cli/init";
    req.headers["Content-Type"] = "application/json";
    req.headers["Authorization"] = "Bearer " + poll_token;
    req.body = Json{{"provider", "zai"}}.dump();
    HttpResponse res;
    FsResult r = net_->request(req, &res);
    if (!r.ok()) return r;
    Json j;
    try { j = Json::parse(res.body); } catch (...) {
      return FsResult::error(ErrCode::kParse, "ZCode OAuth init returned invalid JSON");
    }
    Json d = j.contains("data") ? j["data"] : Json::object();
    f->flow_id = jsonutil::get<std::string>(d, "flow_id", "");
    f->authorize_url = jsonutil::get<std::string>(d, "authorize_url", "");
    f->poll_token = poll_token;
    if (f->flow_id.empty() || f->authorize_url.empty()) {
      return FsResult::error(ErrCode::kRemote,
                             jsonutil::get<std::string>(j, "msg", "ZCode OAuth init failed"));
    }
    std::lock_guard<std::recursive_mutex> lk(mu_);
    flows_[f->id] = f;
    return FsResult::success(Json{{"flowId", f->id},
                                  {"mode", "poll"},
                                  {"url", f->authorize_url},
                                  {"intervalMs", 2500}});
  }

  // Authorization-code + PKCE with a loopback callback.
  f->verifier = random_hex(32);
  f->state = random_hex(16);
  std::string challenge = pkce_challenge(f->verifier);
  f->redirect_uri = "http://localhost:" + std::to_string(def->callback_port) +
                    def->callback_path;

  std::map<std::string, std::string> q;
  q["client_id"] = def->client_id;
  q["response_type"] = "code";
  q["redirect_uri"] = f->redirect_uri;
  q["scope"] = def->scope;
  q["state"] = f->state;
  q["code_challenge"] = challenge;
  q["code_challenge_method"] = "S256";
  if (std::string(def->id) == "claude") q["code"] = "true";
  if (std::string(def->id) == "codex") {
    q["id_token_add_organizations"] = "true";
    q["codex_cli_simplified_flow"] = "true";
    q["originator"] = "codex_cli_rs";
  }
  std::string url = def->auth_url;
  bool first = url.find('?') == std::string::npos;
  for (std::map<std::string, std::string>::iterator it = q.begin(); it != q.end(); ++it) {
    url += (first ? "?" : "&");
    first = false;
    url += url_encode(it->first) + "=" + url_encode(it->second);
  }
  f->authorize_url = url;

  // Loopback callback server. One flow at a time per provider port; a second
  // start fails on bind, which is clearer than silently hijacking the first.
  f->srv.reset(new httplib::Server());
  f->srv->Get(def->callback_path == std::string() ? "/callback" : def->callback_path,
              [f](const httplib::Request& req, httplib::Response& res) {
    std::string code = req.has_param("code") ? req.get_param_value("code") : "";
    std::string state = req.has_param("state") ? req.get_param_value("state") : "";
    std::string err = req.has_param("error") ? req.get_param_value("error") : "";
    if (!err.empty()) f->error = err;
    else {
      f->code = code;
      f->callback_state = state;
    }
    res.set_content(
        "<!doctype html><meta charset=\"utf-8\"><title>FlashSight</title>"
        "<body style=\"font-family:sans-serif;padding:40px\">"
        "<h2>Authorization received</h2><p>You can close this window and "
        "click Verify in FlashSight.</p>"
        "</body>",
        "text/html; charset=utf-8");
    if (f->srv) f->srv->stop();
  });
  if (!f->srv->bind_to_port("127.0.0.1", def->callback_port)) {
    return FsResult::error(
        ErrCode::kIo,
        "callback port " + std::to_string(def->callback_port) +
            " is busy; close the other login window and retry");
  }
  f->server_thread = std::thread([f]() { f->srv->listen_after_bind(); });
  std::lock_guard<std::recursive_mutex> lk(mu_);
  flows_[f->id] = f;
  return FsResult::success(Json{{"flowId", f->id},
                                {"mode", "code"},
                                {"url", f->authorize_url},
                                {"redirectUri", f->redirect_uri},
                                {"intervalMs", 800}});
}

FsResult PlanManager::finish_code(Flow* f, const std::string& code,
                                  const std::string& state) {
  if (!f || !f->def) return FsResult::error(ErrCode::kInternal, "flow missing");
  std::string c = code;
  std::string st = state.empty() ? f->state : state;
  // Claude's web page may append "#state" to the pasted code.
  size_t hash = c.find('#');
  if (hash != std::string::npos) {
    st = c.substr(hash + 1);
    c = c.substr(0, hash);
  }
  return exchange_tokens(f, c);
}

FsResult PlanManager::exchange_tokens(Flow* f, const std::string& code) {
  const PlanProvider* def = f->def;
  HttpRequest req;
  req.method = "POST";
  req.url = def->token_url;
  req.headers["Accept"] = "application/json";
  HttpResponse res;
  if (std::string(def->id) == "claude") {
    Json body = Json{{"code", code},
                     {"state", f->callback_state.empty() ? f->state : f->callback_state},
                     {"grant_type", "authorization_code"},
                     {"client_id", def->client_id},
                     {"redirect_uri", f->redirect_uri},
                     {"code_verifier", f->verifier}};
    req.headers["Content-Type"] = "application/json";
    req.body = body.dump();
  } else {
    std::map<std::string, std::string> form;
    form["grant_type"] = "authorization_code";
    form["code"] = code;
    form["client_id"] = def->client_id;
    form["redirect_uri"] = f->redirect_uri;
    form["code_verifier"] = f->verifier;
    std::string secret = def->client_secret ? def->client_secret : "";
    if (std::string(def->id) == "gemini") secret = google_client_secret();
    if (std::string(def->id) == "gemini" && secret.empty()) {
      return FsResult::error(
          ErrCode::kNotSupported,
          "Google OAuth needs FLASHSIGHT_GOOGLE_CLIENT_SECRET (the client secret "
          "is intentionally not stored in the repository)");
    }
    if (!secret.empty()) form["client_secret"] = secret;
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = form_body(form);
  }
  FsResult r = net_->request(req, &res);
  if (!r.ok()) return r;
  Json j;
  try { j = Json::parse(res.body); } catch (...) {
    return FsResult::error(ErrCode::kParse, "token endpoint returned invalid JSON");
  }
  std::string access = jsonutil::get<std::string>(j, "access_token", "");
  if (access.empty()) {
    std::string msg = jsonutil::get<std::string>(j, "error_description",
                       jsonutil::get<std::string>(j, "error", "token exchange failed"));
    return FsResult::error(ErrCode::kRemote, msg);
  }
  Conn c;
  c.id = "pl_" + random_hex(6);
  c.provider = def->id;
  c.label = def->name;
  c.auth_type = "oauth";
  c.access = access;
  c.refresh = jsonutil::get<std::string>(j, "refresh_token", "");
  c.created_at = now_ms_clock(clock_);
  int64_t expires_in = jsonutil::get_int(j, "expires_in", 0);
  c.expires_at = expires_in > 0 ? c.created_at + expires_in * 1000 : 0;
  c.account_id = jwt_account_id(jsonutil::get<std::string>(j, "id_token", ""));
  if (c.account_id.empty()) {
    c.account_id = jsonutil::get<std::string>(j, "account_id", "");
  }
  c.plan = def->name;
  c.meta = "oauth";
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!doc_.contains("connections")) doc_["connections"] = Json::array();
    doc_["connections"].push_back(c.to_json());
    save();
    f->status = "ready";
  }
  Json pub = conn_public(c);
  pub["connected"] = true;
  if (ready_) ready_(c.to_json());
  return FsResult::success(Json{{"status", "ready"}, {"connection", pub}});
}

FsResult PlanManager::poll_glm(Flow* f) {
  if (!f || !f->def) return FsResult::error(ErrCode::kInternal, "flow missing");
  HttpRequest req;
  req.url = std::string("https://zcode.z.ai/api/v1/oauth/cli/poll/") +
            url_encode(f->flow_id);
  req.headers["Authorization"] = "Bearer " + f->poll_token;
  HttpResponse res;
  FsResult r = net_->request(req, &res);
  if (!r.ok()) return r;
  Json j;
  try { j = Json::parse(res.body); } catch (...) {
    return FsResult::error(ErrCode::kParse, "ZCode poll returned invalid JSON");
  }
  Json d = j.contains("data") ? j["data"] : Json::object();
  std::string status = jsonutil::get<std::string>(d, "status", "");
  if (status == "pending") {
    return FsResult::success(Json{{"status", "pending"}});
  }
  if (status != "ready") {
    f->status = "error";
    return FsResult::error(ErrCode::kRemote, "ZCode authorization was cancelled");
  }
  Json zai = d.contains("zai") ? d["zai"] : Json::object();
  std::string zai_access = jsonutil::get<std::string>(zai, "access_token", "");
  if (zai_access.empty()) zai_access = jsonutil::get<std::string>(d, "accessToken", "");
  if (zai_access.empty()) {
    f->status = "error";
    return FsResult::error(ErrCode::kRemote, "ZCode poll is missing the access token");
  }
  // OAuth token -> business JWT -> coding-plan API key (official CLI chain).
  auto post_json = [this](const std::string& url, const Json& body,
                          const std::string& bearer, Json* out) -> FsResult {
    HttpRequest q;
    q.method = "POST";
    q.url = url;
    q.headers["Content-Type"] = "application/json";
    if (!bearer.empty()) q.headers["Authorization"] = "Bearer " + bearer;
    q.body = body.dump();
    HttpResponse rr;
    FsResult fr = net_->request(q, &rr);
    if (!fr.ok()) return fr;
    try { *out = Json::parse(rr.body); } catch (...) {
      return FsResult::error(ErrCode::kParse, "Z.ai returned invalid JSON");
    }
    return FsResult::success();
  };
  Json login;
  FsResult lr = post_json("https://api.z.ai/api/auth/z/login",
                          Json{{"token", zai_access}}, "", &login);
  if (!lr.ok()) return lr;
  Json login_data = login.contains("data") ? login["data"] : Json::object();
  std::string business = jsonutil::get<std::string>(
      login_data, "access_token",
      jsonutil::get<std::string>(login_data, "accessToken", ""));
  if (business.empty()) {
    f->status = "error";
    return FsResult::error(ErrCode::kRemote, "Z.ai business login is missing access_token");
  }
  HttpRequest ci;
  ci.url = "https://api.z.ai/api/biz/customer/getCustomerInfo";
  ci.headers["Authorization"] = "Bearer " + business;
  HttpResponse cir;
  FsResult cirr = net_->request(ci, &cir);
  if (!cirr.ok()) return cirr;
  Json cij;
  try { cij = Json::parse(cir.body); } catch (...) {
    return FsResult::error(ErrCode::kParse, "Z.ai customer info is invalid JSON");
  }
  Json cid = cij.contains("data") ? cij["data"] : Json::object();
  // The exact org/project keys have varied; accept the documented ones.
  std::string org = jsonutil::get<std::string>(cid, "organization_id", "");
  if (org.empty()) {
    Json orgs = cid.contains("organizations") && cid["organizations"].is_array()
                    ? cid["organizations"] : Json::array();
    if (!orgs.empty()) org = jsonutil::get<std::string>(orgs[0], "id", "");
  }
  std::string proj = jsonutil::get<std::string>(cid, "project_id", "");
  if (proj.empty()) {
    Json ps = cid.contains("projects") && cid["projects"].is_array()
                  ? cid["projects"] : Json::array();
    if (!ps.empty()) proj = jsonutil::get<std::string>(ps[0], "id", "");
  }
  if (org.empty() || proj.empty()) {
    f->status = "error";
    return FsResult::error(ErrCode::kRemote,
                           "Z.ai account has no organization/project for the coding plan");
  }
  std::string list_url = "https://api.z.ai/api/biz/v1/organization/" + org +
                         "/projects/" + proj + "/api_keys";
  HttpRequest lrq;
  lrq.url = list_url;
  lrq.headers["Authorization"] = "Bearer " + business;
  HttpResponse lrr;
  FsResult lrrr = net_->request(lrq, &lrr);
  if (!lrrr.ok()) return lrrr;
  Json keys;
  try { keys = Json::parse(lrr.body); } catch (...) { keys = Json::object(); }
  Json kd = keys.contains("data") ? keys["data"] : keys;
  std::string key_id, secret;
  if (kd.is_array()) {
    for (size_t i = 0; i < kd.size(); ++i) {
      if (jsonutil::get<std::string>(kd[i], "name", "") == "zcode-api-key") {
        key_id = jsonutil::get<std::string>(kd[i], "apiKey", "");
        break;
      }
    }
  }
  if (key_id.empty()) {
    Json create;
    FsResult cr = post_json(list_url, Json{{"name", "zcode-api-key"}}, business, &create);
    if (!cr.ok()) return cr;
    Json cd = create.contains("data") ? create["data"] : create;
    key_id = jsonutil::get<std::string>(cd, "apiKey", "");
  }
  if (key_id.empty()) {
    f->status = "error";
    return FsResult::error(ErrCode::kRemote, "Z.ai did not return a coding-plan API key");
  }
  HttpRequest cq;
  cq.url = list_url + "/copy/" + url_encode(key_id);
  cq.headers["Authorization"] = "Bearer " + business;
  HttpResponse crr;
  FsResult crrr = net_->request(cq, &crr);
  if (!crrr.ok()) return crrr;
  Json cj;
  try { cj = Json::parse(crr.body); } catch (...) {
    return FsResult::error(ErrCode::kParse, "Z.ai key copy is invalid JSON");
  }
  Json cjd = cj.contains("data") ? cj["data"] : cj;
  secret = jsonutil::get<std::string>(cjd, "secretKey", "");
  if (secret.empty()) {
    f->status = "error";
    return FsResult::error(ErrCode::kRemote, "Z.ai key copy is missing secretKey");
  }
  Conn c;
  c.id = "pl_" + random_hex(6);
  c.provider = "glm";
  c.label = f->def->name;
  c.auth_type = "oauth";
  c.access = key_id + "." + secret;
  c.plan = f->def->name;
  c.meta = "zcode-poll";
  c.created_at = now_ms_clock(clock_);
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!doc_.contains("connections")) doc_["connections"] = Json::array();
    doc_["connections"].push_back(c.to_json());
    save();
    f->status = "ready";
  }
  Json pub = conn_public(c);
  if (ready_) ready_(c.to_json());
  return FsResult::success(Json{{"status", "ready"}, {"connection", pub}});
}

FsResult PlanManager::poll(const std::string& flow_id) {
  std::shared_ptr<Flow> f;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::map<std::string, std::shared_ptr<Flow> >::iterator it = flows_.find(flow_id);
    if (it == flows_.end()) return FsResult::error(ErrCode::kNotFound, "login flow expired");
    f = it->second;
  }
  if (f->status == "ready" || f->status == "error") {
    return FsResult::success(Json{{"status", f->status},
                                  {"error", f->error}});
  }
  if (now_ms_clock(clock_) > f->expires_ms) {
    f->status = "error";
    return FsResult::error(ErrCode::kTimeout, "login timed out; start again");
  }
  if (f->def && std::string(f->def->auth) == "poll") {
    FsResult r = poll_glm(f.get());
    if (!r.ok()) return r;
    return r;
  }
  if (f->code.empty() && f->error.empty()) {
    return FsResult::success(Json{{"status", "pending"}});
  }
  if (!f->error.empty()) {
    f->status = "error";
    return FsResult::error(ErrCode::kRemote, f->error);
  }
  FsResult r = finish_code(f.get(), f->code, f->callback_state);
  if (!r.ok()) {
    f->status = "error";
    f->error = r.message;
    return r;
  }
  return r;
}

FsResult PlanManager::complete(const std::string& flow_id, const std::string& code,
                               const std::string& state) {
  std::shared_ptr<Flow> f;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::map<std::string, std::shared_ptr<Flow> >::iterator it = flows_.find(flow_id);
    if (it == flows_.end()) return FsResult::error(ErrCode::kNotFound, "login flow expired");
    f = it->second;
  }
  if (f->def && std::string(f->def->auth) == "poll") return poll(flow_id);
  return finish_code(f.get(), code, state);
}

FsResult PlanManager::cancel(const std::string& flow_id) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  std::map<std::string, std::shared_ptr<Flow> >::iterator it = flows_.find(flow_id);
  if (it != flows_.end()) {
    it->second->status = "cancelled";
    flows_.erase(it);
  }
  return FsResult::success();
}

FsResult PlanManager::disconnect(const std::string& connection_id) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!doc_.contains("connections") || !doc_["connections"].is_array()) {
    return FsResult::success();
  }
  Json out = Json::array();
  bool removed = false;
  for (size_t i = 0; i < doc_["connections"].size(); ++i) {
    if (jsonutil::get<std::string>(doc_["connections"][i], "id", "") == connection_id) {
      removed = true;
      continue;
    }
    out.push_back(doc_["connections"][i]);
  }
  doc_["connections"] = out;
  if (removed) save();
  return FsResult::success();
}

FsResult PlanManager::add_key(const std::string& provider, const std::string& key,
                              const std::string& region, bool remote) {
  // A remote page cannot complete plan login either; keep the whole plans UI
  // local-only so a phone cannot add credentials to this machine.
  if (remote) {
    return FsResult::error(
        ErrCode::kNotSupported,
        "coding-plan setup is disabled for remote connections; do it on the machine running FlashSight");
  }
  const PlanProvider* def = find_plan(provider);
  if (!def) return FsResult::error(ErrCode::kInvalidArg, "unknown plan provider");
  if (key.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty key");
  Conn c;
  c.id = "pl_" + random_hex(6);
  c.provider = provider;
  c.label = def->name;
  c.auth_type = "apikey";
  c.access = key;
  c.region = region;
  c.plan = def->name;
  c.meta = "api-key";
  c.created_at = now_ms_clock(clock_);
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!doc_.contains("connections")) doc_["connections"] = Json::array();
    doc_["connections"].push_back(c.to_json());
    save();
  }
  Json pub = conn_public(c);
  if (ready_) ready_(c.to_json());
  return FsResult::success(Json{{"status", "ready"}, {"connection", pub}});
}

bool PlanManager::touch_provider(const std::string& provider_id, std::string* api_key) {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!doc_.contains("connections") || !doc_["connections"].is_array()) return false;
  for (size_t i = 0; i < doc_["connections"].size(); ++i) {
    Json& j = doc_["connections"][i];
    if (jsonutil::get<std::string>(j, "provider", "") != provider_id) continue;
    Conn c;
    c.provider = provider_id;
    c.access = jsonutil::get<std::string>(j, "accessToken", "");
    c.refresh = jsonutil::get<std::string>(j, "refreshToken", "");
    c.expires_at = jsonutil::get_int(j, "expiresAt", 0);
    if (c.access.empty()) return false;
    // OAuth access tokens are short-lived; refresh shortly before expiry.
    if (c.expires_at > 0 &&
        c.expires_at - now_ms_clock(clock_) < 5 * 60 * 1000) {
      if (refresh_conn(&c).ok()) {
        j["accessToken"] = c.access;
        j["refreshToken"] = c.refresh;
        j["expiresAt"] = c.expires_at;
        save();
      }
    }
    if (api_key) *api_key = c.access;
    return true;
  }
  return false;
}

int64_t PlanManager::resume_at_ms() const {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!doc_.contains("connections") || !doc_["connections"].is_array()) return 0;
  int64_t best = 0;
  for (size_t i = 0; i < doc_["connections"].size(); ++i) {
    const Json& c = doc_["connections"][i];
    if (!c.contains("usage") || !c["usage"].is_object()) continue;
    const Json& q = c["usage"].contains("quotas") ? c["usage"]["quotas"] : Json();
    if (!q.is_array()) continue;
    for (size_t k = 0; k < q.size(); ++k) {
      double remain = jsonutil::get_number(q[k], "remaining", 100);
      int64_t reset = reset_ms(q[k]);
      if (remain <= 0.01 && reset > 0) {
        if (best == 0 || reset < best) best = reset;
      }
    }
  }
  return best > 0 ? best + 5 * 60 * 1000 : 0;
}

FsResult PlanManager::refresh_usage(const std::string& connection_id) {
  std::vector<Json> results;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (!doc_.contains("connections") || !doc_["connections"].is_array()) {
      return FsResult::error(ErrCode::kNotFound, "no plan connections");
    }
    for (size_t i = 0; i < doc_["connections"].size(); ++i) {
      if (!connection_id.empty() &&
          jsonutil::get<std::string>(doc_["connections"][i], "id", "") != connection_id) {
        continue;
      }
      Conn c;
      const Json& j = doc_["connections"][i];
      c.id = jsonutil::get<std::string>(j, "id", "");
      c.provider = jsonutil::get<std::string>(j, "provider", "");
      c.label = jsonutil::get<std::string>(j, "label", "");
      c.access = jsonutil::get<std::string>(j, "accessToken", "");
      c.refresh = jsonutil::get<std::string>(j, "refreshToken", "");
      c.region = jsonutil::get<std::string>(j, "region", "");
      c.account_id = jsonutil::get<std::string>(j, "accountId", "");
      c.expires_at = jsonutil::get_int(j, "expiresAt", 0);
      refresh_conn(&c);
      Json u = refresh_usage_one(&c);
      doc_["connections"][i]["accessToken"] = c.access;
      doc_["connections"][i]["refreshToken"] = c.refresh;
      doc_["connections"][i]["expiresAt"] = c.expires_at;
      doc_["connections"][i]["usage"] = u;
      doc_["connections"][i]["lastUsageAt"] = now_ms_clock(clock_);
      if (ready_) ready_(c.to_json());  // mirror refreshed tokens into provider
      results.push_back(Json{{"id", c.id}, {"usage", u}});
    }
    save();
  }
  if (usage_done_) usage_done_();
  Json arr = Json::array();
  for (size_t i = 0; i < results.size(); ++i) arr.push_back(results[i]);
  return FsResult::success(Json{{"connections", arr},
                                {"resumeAt", resume_at_ms()}});
}

// ---- quota probes ---------------------------------------------------------

Json PlanManager::refresh_usage_one(Conn* c) {
  Json out = Json::object();
  out["quotas"] = Json::array();
  out["fetchedAt"] = now_ms_clock(clock_);
  if (!c || c->access.empty()) {
    out["error"] = "not connected";
    return out;
  }
  if (c->provider == "claude") {
    HttpRequest req;
    req.url = "https://api.anthropic.com/api/oauth/usage?cedar_ember=1";
    req.headers["Authorization"] = "Bearer " + c->access;
    req.headers["anthropic-beta"] = "oauth-2025-04-20";
    req.headers["anthropic-version"] = "2023-06-01";
    req.headers["User-Agent"] = "claude-cli/2.0.0 (external, cli)";
    HttpResponse res;
    FsResult r = net_->request(req, &res);
    if (!r.ok()) { out["error"] = r.message; return out; }
    Json j;
    try { j = Json::parse(res.body); } catch (...) {
      out["error"] = "invalid usage JSON"; return out;
    }
    auto add = [&](const char* key, const Json& w) {
      if (!w.is_object() || !w.contains("utilization")) return;
      double used = jsonutil::get_number(w, "utilization", 0);
      Json q = Json::object();
      q["key"] = key;
      q["used"] = used;
      q["remaining"] = 100.0 - used;
      q["resetAt"] = jsonutil::get<std::string>(w, "resets_at", "");
      out["quotas"].push_back(q);
    };
    add("5h", j.contains("five_hour") ? j["five_hour"] : Json());
    add("week", j.contains("seven_day") ? j["seven_day"] : Json());
    out["plan"] = "Claude Code";
  } else if (c->provider == "codex") {
    HttpRequest req;
    req.url = "https://chatgpt.com/backend-api/wham/usage";
    req.headers["Authorization"] = "Bearer " + c->access;
    req.headers["Accept"] = "application/json";
    req.headers["OpenAI-Beta"] = "codex-1";
    req.headers["originator"] = "codex_cli_rs";
    if (!c->account_id.empty()) req.headers["ChatGPT-Account-ID"] = c->account_id;
    HttpResponse res;
    FsResult r = net_->request(req, &res);
    if (!r.ok()) { out["error"] = r.message; return out; }
    Json j;
    try { j = Json::parse(res.body); } catch (...) {
      out["error"] = "invalid usage JSON"; return out;
    }
    Json rl = j.contains("rate_limit") ? j["rate_limit"] : j;
    auto add = [&](const char* key, const Json& w) {
      if (!w.is_object()) return;
      double used = jsonutil::get_number(w, "used_percent",
                                         jsonutil::get_number(w, "percent_used", 0));
      Json q = Json::object();
      q["key"] = key;
      q["used"] = used;
      q["remaining"] = 100.0 - used;
      q["resetAt"] = jsonutil::get_int(w, "reset_at", 0);
      out["quotas"].push_back(q);
    };
    add("5h", rl.contains("primary_window") ? rl["primary_window"] : Json());
    add("week", rl.contains("secondary_window") ? rl["secondary_window"] : Json());
    out["plan"] = jsonutil::get<std::string>(j, "plan_type", "ChatGPT");
    if (j.contains("rate_limit_reset_credits") &&
        j["rate_limit_reset_credits"].is_object()) {
      out["resetCredits"] = j["rate_limit_reset_credits"].value("available_count", 0);
    }
  } else if (c->provider == "gemini") {
    // loadCodeAssist gives the project id + tier; retrieveUserQuota gives buckets.
    HttpRequest req;
    req.method = "POST";
    req.url = "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist";
    req.headers["Authorization"] = "Bearer " + c->access;
    req.headers["Content-Type"] = "application/json";
    req.body = Json{{"metadata", Json{{"ideType", 9}, {"platform", 5},
                                      {"pluginType", 2}}}}.dump();
    HttpResponse res;
    FsResult r = net_->request(req, &res);
    if (!r.ok()) { out["error"] = r.message; return out; }
    Json info;
    try { info = Json::parse(res.body); } catch (...) { info = Json::object(); }
    std::string project = jsonutil::get<std::string>(info, "cloudaicompanionProject", "");
    out["plan"] = jsonutil::get<std::string>(
        info.contains("currentTier") ? info["currentTier"] : Json::object(), "name", "Gemini");
    if (project.empty()) { out["error"] = "no Code Assist project"; return out; }
    HttpRequest q;
    q.method = "POST";
    q.url = "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota";
    q.headers["Authorization"] = "Bearer " + c->access;
    q.headers["Content-Type"] = "application/json";
    q.body = Json{{"project", project}}.dump();
    HttpResponse qr;
    FsResult qrr = net_->request(q, &qr);
    if (!qrr.ok()) { out["error"] = qrr.message; return out; }
    Json qj;
    try { qj = Json::parse(qr.body); } catch (...) { out["error"] = "invalid quota JSON"; return out; }
    Json buckets = qj.contains("buckets") ? qj["buckets"] : Json::array();
    for (size_t i = 0; i < buckets.size(); ++i) {
      std::string model = jsonutil::get<std::string>(buckets[i], "modelId", "");
      if (model.empty()) continue;
      double frac = jsonutil::get_number(buckets[i], "remainingFraction", 0);
      Json q = Json::object();
      q["key"] = model;
      q["used"] = (1.0 - frac) * 100.0;
      q["remaining"] = frac * 100.0;
      q["resetAt"] = jsonutil::get<std::string>(buckets[i], "resetTime", "");
      out["quotas"].push_back(q);
    }
  } else if (c->provider == "glm") {
    std::string url = c->region == "cn"
                          ? "https://open.bigmodel.cn/api/monitor/usage/quota/limit"
                          : "https://api.z.ai/api/monitor/usage/quota/limit";
    HttpRequest req;
    req.url = url;
    req.headers["Authorization"] = "Bearer " + c->access;
    req.headers["Accept"] = "application/json";
    HttpResponse res;
    FsResult r = net_->request(req, &res);
    if (!r.ok()) { out["error"] = r.message; return out; }
    Json j;
    try { j = Json::parse(res.body); } catch (...) { out["error"] = "invalid quota JSON"; return out; }
    Json d = j.contains("data") ? j["data"] : Json::object();
    Json limits = d.contains("limits") ? d["limits"] : Json::array();
    std::string plan = jsonutil::get<std::string>(d, "level", "");
    out["plan"] = plan.empty() ? "GLM Coding" : plan;
    for (size_t i = 0; i < limits.size(); ++i) {
      std::string type = jsonutil::get<std::string>(limits[i], "type", "");
      if (type != "TOKENS_LIMIT" && type != "CREDIT_LIMIT") continue;
      double pct = jsonutil::get_number(limits[i], "percentage", 0);
      int unit = static_cast<int>(jsonutil::get_int(limits[i], "unit", 0));
      std::string key = unit == 3 ? "5h" : unit == 6 ? "week" : type;
      Json q = Json::object();
      q["key"] = key;
      q["used"] = pct;
      q["remaining"] = 100.0 - pct;
      q["resetAt"] = jsonutil::get_int(limits[i], "nextResetTime", 0);
      out["quotas"].push_back(q);
    }
  } else {
    // Volc/Qwen/Tencent subscription gateways publish no quota API we can
    // verify; do not invent numbers.
    out["plan"] = "Coding Plan";
    out["error"] = "quota API is not available for this plan";
  }
  return out;
}

// ---- token refresh --------------------------------------------------------

FsResult PlanManager::refresh_conn(Conn* c) {
  if (!c || c->refresh.empty()) return FsResult::success();
  if (c->provider == "claude") {
    HttpRequest req;
    req.method = "POST";
    req.url = "https://api.anthropic.com/v1/oauth/token";
    req.headers["Content-Type"] = "application/json";
    Json body = Json{{"grant_type", "refresh_token"},
                     {"refresh_token", c->refresh},
                     {"client_id", "9d1c250a-e61b-44d9-88ed-5944d1962f5e"}};
    req.body = body.dump();
    HttpResponse res;
    FsResult r = net_->request(req, &res);
    if (!r.ok()) return r;
    try {
      Json j = Json::parse(res.body);
      std::string at = jsonutil::get<std::string>(j, "access_token", "");
      if (!at.empty()) c->access = at;
      std::string rt = jsonutil::get<std::string>(j, "refresh_token", "");
      if (!rt.empty()) c->refresh = rt;
      int64_t ex = jsonutil::get_int(j, "expires_in", 0);
      if (ex > 0) c->expires_at = now_ms_clock(clock_) + ex * 1000;
    } catch (...) {
      return FsResult::error(ErrCode::kParse, "refresh response is invalid");
    }
  } else if (c->provider == "codex" || c->provider == "gemini") {
    std::map<std::string, std::string> form;
    form["grant_type"] = "refresh_token";
    form["refresh_token"] = c->refresh;
    if (c->provider == "codex") {
      form["client_id"] = "app_EMoamEEZ73f0CkXaXp7hrann";
      form["scope"] = "openid profile email offline_access";
    } else {
      form["client_id"] = "681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com";
      std::string secret = google_client_secret();
      if (secret.empty()) {
        return FsResult::error(
            ErrCode::kNotSupported,
            "Google OAuth refresh needs FLASHSIGHT_GOOGLE_CLIENT_SECRET");
      }
      form["client_secret"] = secret;
    }
    HttpRequest req;
    req.method = "POST";
    req.url = c->provider == "codex" ? "https://auth.openai.com/oauth/token"
                                     : "https://oauth2.googleapis.com/token";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = form_body(form);
    HttpResponse res;
    FsResult r = net_->request(req, &res);
    if (!r.ok()) return r;
    try {
      Json j = Json::parse(res.body);
      std::string at = jsonutil::get<std::string>(j, "access_token", "");
      if (!at.empty()) c->access = at;
      std::string rt = jsonutil::get<std::string>(j, "refresh_token", "");
      if (!rt.empty()) c->refresh = rt;
      int64_t ex = jsonutil::get_int(j, "expires_in", 0);
      if (ex > 0) c->expires_at = now_ms_clock(clock_) + ex * 1000;
    } catch (...) {
      return FsResult::error(ErrCode::kParse, "refresh response is invalid");
    }
  }
  return FsResult::success();
}

}  // namespace fs
