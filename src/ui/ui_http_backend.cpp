// ui_http_backend.cpp - browser backend: talks to the local exe over HTTP.
//
// Compiled only for the wasm build. Requests must not block the render loop,
// so call() reports failure immediately and call_async() uses an EM_JS fetch
// wrapper that invokes a C callback on completion. Events are polled from
// /api/events on a timer (poll()).
#include "ui_backend.hpp"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fs_json.hpp"
#include "ui_dom.hpp"

namespace fs {
namespace ui {

namespace {

// JS side: pending XHR/fetch callbacks keyed by an integer id.
// __fsApiFetch(id, url, method, bodyPtr) starts the request and, on completion,
// calls Module._fs_api_done(id, okFlag, bodyPtr).
EM_JS(void, fs_api_fetch, (int id, const char* url, const char* method,
                           const char* body), {
  var u = UTF8ToString(url), m = UTF8ToString(method), b = UTF8ToString(body);
  fetch(u, {
    method: m,
    headers: { 'Content-Type': 'application/json' },
    body: (m === 'GET' || m === 'HEAD') ? undefined : b,
  }).then(function (r) { return r.text(); })
    .then(function (text) {
      var len = lengthBytesUTF8(text) + 1;
      var ptr = _malloc(len);
      stringToUTF8(text, ptr, len);
      Module._fs_api_done(id, 1, ptr);
      _free(ptr);
    })
    .catch(function () {
      Module._fs_api_done(id, 0, 0);
    });
});

std::map<int, BackendDone>* g_pending = nullptr;
int g_seq = 0;
IUiBackend* g_backend = nullptr;  // the platform backend (diagnostics)

}  // namespace

// called from JS when a fetch completes.
// Two response shapes reach here:
//   * /api/call   -> {"ok":bool,"data":{...},"code":"...","message":"..."}
//   * /api/events -> {"events":[...],"next":N}   (no ok wrapper)
extern "C" EMSCRIPTEN_KEEPALIVE void fs_api_done(int id, int ok, const char* body) {
  if (!g_pending) return;
  std::map<int, BackendDone>::iterator it = g_pending->find(id);
  if (it == g_pending->end()) return;
  BackendDone done = it->second;
  g_pending->erase(it);
  BackendResult r;
  r.ok = ok != 0;
  if (!body || !*body) {
    r.ok = false;
    r.code = "network";
    r.message = "request failed";
    if (done) done(r);
    return;
  }
  Json j;
  try {
    j = Json::parse(body);
  } catch (...) {
    r.ok = false;
    r.code = "parse";
    r.message = "bad response";
    if (done) done(r);
    return;
  }
  if (j.is_object() && j.contains("ok")) {
    // /api/call shape
    r.ok = jsonutil::get<bool>(j, "ok", false);
    r.data = j.contains("data") ? j["data"] : Json::object();
    r.code = jsonutil::get<std::string>(j, "code", "");
    r.message = jsonutil::get<std::string>(j, "message", "");
  } else {
    // raw payload (e.g. /api/events)
    r.ok = true;
    r.data = j;
  }
  if (done) done(r);
}

class HttpBackend : public IUiBackend {
 public:
  HttpBackend() {
    g_pending = &pending_;
    last_event_id_ = 0;
  }
  ~HttpBackend() override {
    if (g_pending == &pending_) g_pending = nullptr;
  }

  // Synchronous path is unavailable in the browser (would block the UI).
  BackendResult call(const std::string& method, const Json& args) override {
    BackendResult r;
    r.ok = false;
    r.code = "async-required";
    r.message = "use call_async in the browser build (" + method + ")";
    return r;
  }

  void call_async(const std::string& method, const Json& args, BackendDone done) override {
    Json body = Json::object();
    body["method"] = method;
    body["args"] = args;
    int id = ++g_seq;
    pending_[id] = done;
    fs_api_fetch(id, "/api/call", "POST", body.dump().c_str());
  }

  std::function<void()> subscribe(BackendEvent fn) override {
    subs_.push_back(fn);
    size_t id = subs_.size() - 1;
    return [this, id]() {
      if (id < subs_.size()) subs_[id] = nullptr;
    };
  }

  // Poll /api/events?since=N; deliver each event to subscribers.
  void poll() override {
    std::string url = "/api/events?since=" + std::to_string(last_event_id_);
    int id = ++g_seq;
    pending_[id] = [this](const BackendResult& r) {
      if (!r.ok || !r.data.contains("events")) return;
      const Json& evs = r.data["events"];
      if (r.data.contains("next")) {
        last_event_id_ = jsonutil::get_int(r.data, "next", last_event_id_);
      }
      for (size_t i = 0; i < evs.size(); ++i) {
        std::string name = jsonutil::get<std::string>(evs[i], "event", "");
        Json payload = evs[i].contains("payload") ? evs[i]["payload"] : Json::object();
        ++delivered_;
        for (size_t k = 0; k < subs_.size(); ++k) {
          if (subs_[k]) subs_[k](name, payload);
        }
      }
    };
    fs_api_fetch(id, url.c_str(), "GET", "");
  }

  // diagnostics (debug export)
  int subscriber_count() const { return static_cast<int>(subs_.size()); }
  int64_t last_event_id() const { return last_event_id_; }
  int delivered() const { return delivered_; }

 private:
  std::map<int, BackendDone> pending_;
  std::vector<BackendEvent> subs_;
  int64_t last_event_id_ = 0;
  int delivered_ = 0;
};

IDom* make_platform_dom();  // ui_dom_wasm.cpp

// Factory used by the wasm entry point.
IUiBackend* make_platform_backend() {
  g_backend = new HttpBackend();
  return g_backend;
}

// diagnostics (debug export): subscriber count / delivered events / last id
extern "C" EMSCRIPTEN_KEEPALIVE int fs_ui_backend_debug(int* last_id, int* delivered) {
  HttpBackend* hb = static_cast<HttpBackend*>(g_backend);
  if (!hb) return -1;
  if (last_id) *last_id = static_cast<int>(hb->last_event_id());
  if (delivered) *delivered = hb->delivered();
  return hb->subscriber_count();
}

}  // namespace ui
}  // namespace fs

#endif  // __EMSCRIPTEN__
