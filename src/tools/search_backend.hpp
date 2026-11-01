// search_backend.hpp - web search over a configurable HTTP API
//
// WHY THIS IS A SEPARATE MODULE
// "Search" has no universal protocol: Tavily, Brave and SearXNG each take a
// different request shape and return a different document. The tool layer must
// not know about any of them, and the provider layer is about models, not
// search. So the tool forwards {query, count} to a callback that this module
// builds, and the callback is wired only when the user has configured a backend.
//
// BEFORE THIS FILE the tool sent `{search:true}` into the generic fetch callback,
// which reads `url` - so every call failed with "empty url". That is the "search
// isn't wired" the user hit.
#pragma once

#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {
namespace search {

// Result of one search.
struct Hit {
  std::string title;
  std::string url;
  std::string snippet;
};

// Configuration, read from settings. Empty `provider` means "not configured".
struct Config {
  std::string provider;   // tavily | brave | searxng | custom
  std::string endpoint;   // full URL; empty = use the provider's default
  std::string api_key;
  int max_results = 5;
};

// Parse a settings JSON blob (the three keys merged) into Config.
Config config_from_settings(const Json& provider, const Json& endpoint,
                            const Json& api_key, const Json& max_results);

// Build the HTTP request for one query. Returns {method, url, headers, body}.
// `ok` is false when the configuration is unusable, with `error` explaining
// what is missing - the model gets that text, so it must say what to configure.
struct HttpCall {
  bool ok = false;
  std::string error;
  std::string method = "GET";
  std::string url;
  Json headers = Json::object();
  std::string body;
};
HttpCall build_request(const Config& cfg, const std::string& query, int count);

// Parse a provider's response body into hits. Unknown providers get a generic
// best-effort parse (common field names), so a user's custom endpoint that
// follows the usual shape still works.
struct Parsed {
  bool ok = false;
  std::string error;
  std::vector<Hit> hits;
};
Parsed parse_response(const Config& cfg, int http_status, const std::string& body);

// One-line description of what a provider needs (shown in settings and in the
// error the model sees).
std::string provider_hint(const std::string& provider);

}  // namespace search
}  // namespace fs
