// ui_server.cpp - local HTTP server: embedded assets + backend API.
#include "ui_server.hpp"

#include <atomic>
#include <thread>

#include "httplib.h"
#include "ui_api.hpp"
#include "ui_backend.hpp"
#include "ui_assets_gen.hpp"
#include "core/fs_api.hpp"

namespace fs {
namespace ui {

struct UiServer::Impl {
  std::unique_ptr<httplib::Server> svr;
  std::thread th;
  std::atomic<bool> running{false};
  int port = 0;
  std::unique_ptr<UiApi> api;
  // event ring: {id, event, payload}; id is monotonic
  std::mutex mu;
  std::deque<std::pair<int64_t, std::pair<std::string, Json> > > events;
  int64_t next_event_id = 1;
  int listener_id = -1;
  FsApi* bound_api = nullptr;
};

namespace {
constexpr size_t kEventRingMax = 512;
}

UiServer::UiServer() : impl_(new Impl()) {}

UiServer::~UiServer() { stop(); }

void UiServer::bind_api(FsApi* api) {
  impl_->bound_api = api;
  if (!api) return;
  impl_->api.reset(new UiApi(api));
  // collect events into the ring (the UI polls /api/events)
  Impl* im = impl_.get();
  impl_->listener_id = api->add_event_listener([im](const std::string& e, const Json& p) {
    std::lock_guard<std::mutex> lk(im->mu);
    im->events.push_back(std::make_pair(im->next_event_id++,
                                        std::make_pair(e, p)));
    while (im->events.size() > kEventRingMax) im->events.pop_front();
  });
}

Json UiServer::drain_events(int64_t since, int64_t* next) {
  Json arr = Json::array();
  int64_t max_id = 0;
  std::lock_guard<std::mutex> lk(impl_->mu);
  for (size_t i = 0; i < impl_->events.size(); ++i) {
    const std::pair<int64_t, std::pair<std::string, Json> >& ev = impl_->events[i];
    if (ev.first <= since) continue;
    Json o = Json::object();
    o["id"] = ev.first;
    o["event"] = ev.second.first;
    o["payload"] = ev.second.second;
    arr.push_back(o);
    if (ev.first > max_id) max_id = ev.first;
  }
  // Cursor must be the highest id actually returned. Using next_event_id here
  // would skip the very first event: an empty ring reports next=1, the client
  // then polls since=1 and never sees event id 1.
  if (next) *next = max_id > since ? max_id : since;
  return arr;
}

FsResult UiServer::start(int port) {
  if (impl_->running.load()) return FsResult::success();
  impl_->svr.reset(new httplib::Server());

  // ---- backend API ----
  impl_->svr->Post("/api/call", [this](const httplib::Request& req, httplib::Response& res) {
    Json body;
    try {
      body = Json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content("{\"ok\":false,\"code\":\"parse\",\"message\":\"bad json\"}",
                      "application/json");
      return;
    }
    std::string method = jsonutil::get<std::string>(body, "method", "");
    Json args = body.contains("args") ? body["args"] : Json::object();
    if (!impl_->api) {
      res.status = 503;
      res.set_content("{\"ok\":false,\"code\":\"not-supported\",\"message\":\"no api bound\"}",
                      "application/json");
      return;
    }
    BackendResult br = to_backend_result(impl_->api->call(method, args));
    Json out = Json::object();
    out["ok"] = br.ok;
    if (br.ok) {
      out["data"] = br.data;
    } else {
      out["code"] = br.code;
      out["message"] = br.message;
      out["data"] = br.data;
    }
    res.set_content(out.dump(), "application/json");
  });

  impl_->svr->Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
    int64_t since = 0;
    if (req.has_param("since")) {
      try {
        since = std::stoll(req.get_param_value("since"));
      } catch (...) {
        since = 0;
      }
    }
    int64_t next = 0;
    Json events = drain_events(since, &next);
    Json out = Json::object();
    out["events"] = events;
    out["next"] = next;
    // A payload with invalid UTF-8 would make dump() throw and turn the whole
    // event stream into a 500, stalling the UI forever. Encoding is fixed at
    // the source (fs_encoding), but a bad payload must never be fatal here.
    std::string body;
    try {
      body = out.dump();
    } catch (...) {
      body = "{\"events\":[],\"next\":" + std::to_string(next) + "}";
    }
    res.set_content(body, "application/json");
  });

  // ---- file upload (chat attachments) ----
  // The browser posts multipart/form-data; the file is written into the
  // project's .flashsight/uploads/ so the model's file tools can read it by
  // path (the model never sees browser-side bytes). Returns the saved paths.
  impl_->svr->Post("/api/upload", [this](const httplib::Request& req,
                                         httplib::Response& res) {
    if (!impl_->bound_api) {
      res.status = 503;
      res.set_content("{\"ok\":false,\"code\":\"not-supported\"}", "application/json");
      return;
    }
    std::string pid = req.has_param("pid") ? req.get_param_value("pid") : "";
    if (req.form.has_field("pid")) pid = req.form.get_field("pid");
    Json saved = Json::array();
    std::string error;
    // iterate the parts in the order the browser sent them (the multipart type
    // is insertion-ordered on purpose)
    const httplib::FormFiles& files = req.form.files;
    for (httplib::FormFiles::const_iterator it = files.begin(); it != files.end();
         ++it) {
      const httplib::FormData& f = it->second;
      if (f.name != "file") continue;
      FsResult r = impl_->bound_api->project_save_upload(pid, f.filename, f.content);
      if (!r.ok()) {
        error = r.message;
        continue;
      }
      saved.push_back(r.data);
    }
    Json out = Json::object();
    out["ok"] = error.empty() || !saved.empty();
    Json data = Json::object();
    data["files"] = saved;
    if (!error.empty()) data["error"] = error;
    out["data"] = data;
    res.set_content(out.dump(), "application/json");
  });

  // ---- static assets (fallback; keep last so /api/* wins) ----
  impl_->svr->Get("/(.*)", [](const httplib::Request& req, httplib::Response& res) {
    std::string path = req.matches.size() > 1 ? req.matches[1].str() : "";
    if (path.empty() || path == "/") path = "index.html";
    // The bundle is rebuilt constantly during development, but the filenames
    // never change (fs_ui.wasm, style.css). Without no-store the browser serves
    // its cached copy after a rebuild and the app behaves like the fix was never
    // made - which is exactly how a framework patch "did not work".
    res.set_header("Cache-Control", "no-store, max-age=0");
    for (size_t i = 0; i < assets::kFiles().size(); ++i) {
      const assets::Asset& a = assets::kFiles()[i];
      if (path == a.name) {
        res.set_content(reinterpret_cast<const char*>(a.data), a.size, a.mime);
        return;
      }
    }
    res.status = 404;
    res.set_content("not found", "text/plain");
  });

  // 端口：0 = 让系统分配
  int p = impl_->svr->bind_to_any_port("127.0.0.1", 0);
  if (port > 0) {
    p = port;
    if (!impl_->svr->bind_to_port("127.0.0.1", port)) {
      return FsResult::error(ErrCode::kIo, "cannot bind port " + std::to_string(port));
    }
  }
  impl_->port = p;
  impl_->running.store(true);
  impl_->th = std::thread([this]() { impl_->svr->listen_after_bind(); });
  // 等 listen 就绪（简单轮询）
  for (int i = 0; i < 100 && !impl_->svr->is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return FsResult::success();
}

void UiServer::stop() {
  if (impl_ && impl_->running.load()) {
    impl_->svr->stop();
    if (impl_->th.joinable()) impl_->th.join();
    impl_->running.store(false);
  }
  if (impl_ && impl_->bound_api && impl_->listener_id >= 0) {
    impl_->bound_api->remove_event_listener(impl_->listener_id);
    impl_->listener_id = -1;
  }
}

bool UiServer::running() const { return impl_ && impl_->running.load(); }

std::string UiServer::url() const {
  if (!impl_) return "";
  return "http://127.0.0.1:" + std::to_string(impl_->port) + "/";
}

}  // namespace ui
}  // namespace fs
