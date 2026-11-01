// search_backend.cpp - request/response shapes for the supported search APIs
#include "search_backend.hpp"

namespace fs {
namespace search {

namespace {

std::string get_str(const Json& j, const char* k, const char* def = "") {
  return jsonutil::get<std::string>(j, k, def);
}

// Percent-encode a query for a URL. Everything outside [A-Za-z0-9-_.~] is
// escaped, which is what both Brave and SearXNG expect for `q=`.
std::string url_encode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

// The default endpoint for each provider. Kept here (not in settings) so a user
// only has to pick a provider and paste a key.
std::string default_endpoint(const std::string& provider) {
  if (provider == "tavily") return "https://api.tavily.com/search";
  if (provider == "brave") return "https://api.search.brave.com/res/v1/web/search";
  return "";
}

}  // namespace

std::string provider_hint(const std::string& provider) {
  if (provider == "tavily") {
    return "Tavily: needs an API key (https://tavily.com). Endpoint defaults to "
           "api.tavily.com/search.";
  }
  if (provider == "brave") {
    return "Brave Search: needs an API key (https://brave.com/search/api). "
           "Endpoint defaults to api.search.brave.com.";
  }
  if (provider == "searxng") {
    return "SearXNG: a self-hosted instance. Endpoint is required (e.g. "
           "https://searx.example.org/search); no API key.";
  }
  if (provider == "custom") {
    return "Custom: any endpoint that accepts {query, count} and returns a JSON "
           "list of results. Endpoint is required.";
  }
  return "No search backend configured. Pick one in Settings > Model > Search.";
}

Config config_from_settings(const Json& provider, const Json& endpoint,
                            const Json& api_key, const Json& max_results) {
  Config c;
  if (provider.is_string()) c.provider = provider.get<std::string>();
  if (endpoint.is_string()) c.endpoint = endpoint.get<std::string>();
  if (api_key.is_string()) c.api_key = api_key.get<std::string>();
  if (max_results.is_number()) {
    c.max_results = static_cast<int>(max_results.get<int64_t>());
  }
  if (c.max_results <= 0) c.max_results = 5;
  if (c.max_results > 20) c.max_results = 20;
  if (c.endpoint.empty()) c.endpoint = default_endpoint(c.provider);
  return c;
}

HttpCall build_request(const Config& cfg, const std::string& query, int count) {
  HttpCall call;
  if (cfg.provider.empty()) {
    call.error = provider_hint("");
    return call;
  }
  if (cfg.endpoint.empty()) {
    call.error = "search endpoint is empty. " + provider_hint(cfg.provider);
    return call;
  }
  if (query.empty()) {
    call.error = "empty query";
    return call;
  }
  if (count <= 0) count = cfg.max_results;

  if (cfg.provider == "tavily") {
    if (cfg.api_key.empty()) {
      call.error = "Tavily needs an API key. " + provider_hint("tavily");
      return call;
    }
    call.ok = true;
    call.method = "POST";
    call.url = cfg.endpoint;
    call.headers["Content-Type"] = "application/json";
    call.headers["Authorization"] = "Bearer " + cfg.api_key;
    Json body = Json::object();
    body["query"] = query;
    body["max_results"] = count;
    body["include_answer"] = false;
    call.body = body.dump();
    return call;
  }

  if (cfg.provider == "brave") {
    if (cfg.api_key.empty()) {
      call.error = "Brave Search needs an API key. " + provider_hint("brave");
      return call;
    }
    call.ok = true;
    call.method = "GET";
    call.url = cfg.endpoint + "?q=" + url_encode(query) + "&count=" +
               std::to_string(count);
    call.headers["Accept"] = "application/json";
    call.headers["X-Subscription-Token"] = cfg.api_key;
    return call;
  }

  if (cfg.provider == "searxng") {
    // SearXNG: GET /search?q=...&format=json
    call.ok = true;
    call.method = "GET";
    call.url = cfg.endpoint + (cfg.endpoint.find('?') == std::string::npos ? "?" : "&") +
               "q=" + url_encode(query) + "&format=json";
    call.headers["Accept"] = "application/json";
    if (!cfg.api_key.empty()) call.headers["Authorization"] = "Bearer " + cfg.api_key;
    return call;
  }

  // custom: POST {query, count}; the user's endpoint decides the rest.
  call.ok = true;
  call.method = "POST";
  call.url = cfg.endpoint;
  call.headers["Content-Type"] = "application/json";
  if (!cfg.api_key.empty()) call.headers["Authorization"] = "Bearer " + cfg.api_key;
  Json body = Json::object();
  body["query"] = query;
  body["count"] = count;
  call.body = body.dump();
  return call;
}

Parsed parse_response(const Config& cfg, int http_status, const std::string& body) {
  Parsed out;
  if (http_status < 200 || http_status >= 300) {
    out.error = "search backend returned http " + std::to_string(http_status);
    if (!body.empty()) {
      std::string flat = body.substr(0, 300);
      out.error += ": " + flat;
    }
    return out;
  }
  Json j;
  try {
    j = Json::parse(body);
  } catch (...) {
    out.error = "search backend returned non-JSON (is the endpoint right?)";
    return out;
  }
  // Each provider nests results differently; try the known shapes in order and
  // fall back to a generic scan so a custom endpoint usually still works.
  Json list = Json::array();
  if (j.contains("results") && j["results"].is_array()) {
    list = j["results"];                       // tavily, searxng
  } else if (j.contains("web") && j["web"].is_object() &&
             j["web"].contains("results") && j["web"]["results"].is_array()) {
    list = j["web"]["results"];                // brave
  } else if (j.contains("data") && j["data"].is_array()) {
    list = j["data"];                          // common REST wrapper
  } else if (j.is_array()) {
    list = j;                                  // bare array
  } else if (j.contains("items") && j["items"].is_array()) {
    list = j["items"];
  }
  if (list.empty()) {
    // A valid response with no hits is not an error: the query may simply have
    // no results, and the model should see that rather than a failure.
    out.ok = true;
    return out;
  }
  for (auto& r : list) {
    if (!r.is_object()) continue;
    Hit h;
    h.title = get_str(r, "title");
    if (h.title.empty()) h.title = get_str(r, "name");
    h.url = get_str(r, "url");
    if (h.url.empty()) h.url = get_str(r, "link");
    if (h.url.empty()) h.url = get_str(r, "href");
    h.snippet = get_str(r, "content");
    if (h.snippet.empty()) h.snippet = get_str(r, "snippet");
    if (h.snippet.empty()) h.snippet = get_str(r, "description");
    if (h.snippet.empty()) h.snippet = get_str(r, "text");
    if (h.title.empty() && h.url.empty() && h.snippet.empty()) continue;
    out.hits.push_back(h);
  }
  out.ok = true;
  return out;
}

}  // namespace search
}  // namespace fs
