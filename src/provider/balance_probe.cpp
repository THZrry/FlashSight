// balance_probe.cpp - see header
#include "balance_probe.hpp"

#include <cstdio>
#include <cstring>

#include "common/fs_json.hpp"

namespace fs {
namespace {

std::string lower_ascii(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
  }
  return s;
}

// scheme://host[:port] -> host (lowercase)
std::string url_host_of(const std::string& url) {
  size_t p = url.find("://");
  std::string rest = p == std::string::npos ? url : url.substr(p + 3);
  size_t slash = rest.find('/');
  std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
  size_t at = host.find('@');
  if (at != std::string::npos) host = host.substr(at + 1);
  size_t colon = host.find(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  return lower_ascii(host);
}

bool host_contains(const std::string& host, const char* needle) {
  return host.find(needle) != std::string::npos;
}

// The vendor's API root: the configured base URL without a trailing /v1 or
// /v1beta (their account endpoints live at the root, not under the chat path).
std::string api_root_of(const std::string& base_url) {
  std::string u = base_url;
  while (!u.empty() && u[u.size() - 1] == '/') u.erase(u.size() - 1);
  const char* tails[] = {"/v1beta", "/v1"};
  for (size_t t = 0; t < 2; ++t) {
    size_t n = std::strlen(tails[t]);
    if (u.size() >= n && u.compare(u.size() - n, n, tails[t]) == 0) {
      u.erase(u.size() - n);
      break;
    }
  }
  return u;
}

std::string num_to_str(const Json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v.get<double>());
    return std::string(buf);
  }
  return std::string();
}

BalanceProbe fail(const std::string& why) {
  BalanceProbe p;
  p.status = "failed";
  p.note = "获取失败：" + why;
  return p;
}

BalanceProbe unsupported() {
  BalanceProbe p;
  p.status = "unsupported";
  p.note = "该供应商没有公开的余额接口";
  return p;
}

}  // namespace

BalanceProbe probe_balance(const ProviderConn& conn, const NetClient* net) {
  if (!net) return fail("network unavailable");
  std::string host = url_host_of(conn.base_url);
  std::string root = api_root_of(conn.base_url);
  std::string url;
  enum Vendor { kDeepSeek, kOpenRouter, kMoonshot, kSiliconFlow, kOpenAI, kNone };
  Vendor vendor = kNone;
  if (host_contains(host, "deepseek")) {
    vendor = kDeepSeek;
    url = "https://api.deepseek.com/user/balance";
  } else if (host_contains(host, "openrouter")) {
    vendor = kOpenRouter;
    url = root + "/api/v1/credits";
  } else if (host_contains(host, "moonshot")) {
    vendor = kMoonshot;
    url = root + "/v1/users/me/balance";
  } else if (host_contains(host, "siliconflow")) {
    vendor = kSiliconFlow;
    url = root + "/v1/user/info";
  } else if (host_contains(host, "openai.com") || host_contains(host, "api2d") ||
             host_contains(host, "ohmygpt")) {
    // OpenAI's legacy billing endpoint; it is being retired, so a 404 is
    // reported as a failure rather than pretended away.
    vendor = kOpenAI;
    url = root + "/v1/dashboard/billing/credit_grants";
  } else {
    return unsupported();
  }
  if (conn.api_key.empty()) return fail("未配置 API Key");

  HttpRequest req;
  req.method = "GET";
  req.url = url;
  req.timeout_ms = 5000;  // a balance check must not hold the UI hostage
  req.size_cap = 256 * 1024;
  req.headers["Accept"] = "application/json";
  req.headers["Authorization"] = "Bearer " + conn.api_key;
  for (std::map<std::string, std::string>::const_iterator it =
           conn.custom_headers.begin();
       it != conn.custom_headers.end(); ++it) {
    req.headers[it->first] = it->second;
  }
  HttpResponse resp;
  FsResult nr = net->request(req, &resp);
  if (!nr.ok()) return fail(resp.error.empty() ? nr.message : resp.error);
  if (resp.status < 200 || resp.status >= 300) {
    return fail("HTTP " + std::to_string(resp.status));
  }
  Json j;
  try {
    j = Json::parse(resp.body);
  } catch (...) {
    return fail("返回内容不是 JSON");
  }

  if (vendor == kDeepSeek) {
    // {"is_available":true,"balance_infos":[
    //   {"currency":"CNY","total_balance":"1.00","granted_balance":"0.00",
    //    "topped_up_balance":"1.00"}]}
    const Json& infos = j.contains("balance_infos") ? j["balance_infos"] : Json();
    if (!infos.is_array() || infos.empty()) return fail("返回里没有余额字段");
    BalanceProbe p;
    p.status = "ok";
    p.currency = jsonutil::get<std::string>(infos[0], "currency", "");
    p.balance = num_to_str(infos[0].contains("total_balance")
                               ? infos[0]["total_balance"] : Json());
    if (p.balance.empty()) p.balance = "0.00";
    p.used = num_to_str(infos[0].contains("granted_balance")
                            ? infos[0]["granted_balance"] : Json());
    return p;
  }
  if (vendor == kOpenRouter) {
    // {"data":{"total_credits":10,"total_usage":2.5}}
    const Json& d = j.contains("data") ? j["data"] : Json();
    double credits = d.is_object() ? jsonutil::get_number(d, "total_credits", -1) : -1;
    double used = d.is_object() ? jsonutil::get_number(d, "total_usage", -1) : -1;
    if (credits < 0) return fail("返回里没有 credits 字段");
    BalanceProbe p;
    p.status = "ok";
    p.currency = "USD";
    p.limit = num_to_str(Json(credits));
    if (used >= 0) {
      p.used = num_to_str(Json(used));
      p.balance = num_to_str(Json(credits - used));
    } else {
      p.balance = num_to_str(Json(credits));
    }
    return p;
  }
  if (vendor == kMoonshot) {
    // {"data":{"available_balance":12.34}}
    const Json& d = j.contains("data") ? j["data"] : Json();
    std::string bal = d.is_object() ? num_to_str(d.contains("available_balance")
                                                     ? d["available_balance"] : Json())
                                    : std::string();
    if (bal.empty()) return fail("返回里没有余额字段");
    BalanceProbe p;
    p.status = "ok";
    p.currency = "CNY";
    p.balance = bal;
    return p;
  }
  if (vendor == kSiliconFlow) {
    // {"data":{"balance":"1.00","totalBalance":"10.00"}}
    const Json& d = j.contains("data") ? j["data"] : Json();
    std::string bal = d.is_object()
                          ? num_to_str(d.contains("totalBalance")
                                           ? d["totalBalance"]
                                           : (d.contains("balance") ? d["balance"]
                                                                    : Json()))
                          : std::string();
    if (bal.empty()) return fail("返回里没有余额字段");
    BalanceProbe p;
    p.status = "ok";
    p.currency = "CNY";
    p.balance = bal;
    return p;
  }
  // OpenAI-style credit grants: {"total_available":4.2,"total_granted":10}
  std::string avail =
      num_to_str(j.contains("total_available") ? j["total_available"] : Json());
  if (avail.empty()) return fail("返回里没有余额字段");
  BalanceProbe p;
  p.status = "ok";
  p.currency = "USD";
  p.balance = avail;
  p.used = num_to_str(j.contains("total_used") ? j["total_used"] : Json());
  p.limit = num_to_str(j.contains("total_granted") ? j["total_granted"] : Json());
  return p;
}

}  // namespace fs
