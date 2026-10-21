// ui_server.cpp - local HTTP server: embedded assets + backend API.
#include "ui_server.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <random>
#include <set>
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
  std::string host = "127.0.0.1";
  std::string password;   // empty = no auth
  // Issued session tokens. In-memory only: a restart invalidates every remote
  // session, which is the right default for a password that may just have
  // changed.
  std::set<std::string> tokens;
  std::mutex token_mu;
  // Set while a rebind is in flight (the swap runs on a helper thread). The
  // settings page polls status right after writing, and without this it would
  // read the OLD listener and report the old port as if nothing happened.
  std::atomic<bool> rebinding{false};
  // Last rebind failure ("address already in use", "cannot bind ..."), shown in
  // the settings page. Silence here meant a failed port change looked like the
  // setting simply had no effect.
  std::mutex err_mu;
  std::string last_error;
  // Per-LISTENER stop flag. An SSE stream never ends on its own - it waits for
  // the next event - and httplib's listen loop joins its thread pool on
  // shutdown, so a connected page keeps one worker busy and stop() would block
  // forever. The SSE handler checks its own generation's flag and returns.
  //
  // It MUST be per generation, not one flag on Impl: during a rebind the old and
  // the new listener overlap briefly (that overlap is what gives the page time to
  // follow "server.moved"), and a shared flag would kill the new listener's
  // streams the moment the old one was retired.
  std::shared_ptr<std::atomic<bool> > stop_flag =
      std::make_shared<std::atomic<bool> >(false);
  // ---- rebind serialization ----
  //
  // restart() used to spawn a NEW detached thread per call. Changing the host and
  // then the port (or typing a port twice) gave two helpers that both entered
  // stop(); the second join() on an already-joined std::thread throws
  // std::system_error, and because it happened inside a detached thread nobody
  // caught it -> std::terminate, i.e. the app vanished with no message.
  //
  // One persistent worker now owns the rebind. Callers only publish the latest
  // desired settings (coalescing: the last write wins, which is exactly right for
  // a form field) and signal the worker.
  std::thread rebind_worker;
  std::mutex rebind_mu;
  std::condition_variable rebind_cv;
  bool rebind_has_request = false;
  bool rebind_quit = false;
  int rebind_port = 0;
  std::string rebind_host;
  std::string rebind_password;
  // Back-pointer for the worker lambda (it cannot capture `this` from restart()
  // because the worker outlives that call).
  UiServer* owner = nullptr;
  // The pre-routing handler runs on httplib's worker threads, so token lookups
  // need the lock (token_mu). Declared as a member function so the lambda above
  // can call it while the struct is complete.
  bool valid_token_locked(const std::string& t) {
    std::lock_guard<std::mutex> lk(token_mu);
    return tokens.count(t) > 0;
  }
  std::unique_ptr<UiApi> api;
  // event ring: {id, event, payload}; id is monotonic
  std::mutex mu;
  // Signalled on every new event. An SSE connection waits on this instead of
  // sleeping, so a delta reaches the browser the moment it is produced.
  std::condition_variable cv;
  std::deque<std::pair<int64_t, std::pair<std::string, Json> > > events;
  int64_t next_event_id = 1;
  int listener_id = -1;
  FsApi* bound_api = nullptr;
};

namespace {
// The ring has to hold a whole streaming reply: a fast model emits hundreds of
// deltas between two UI polls, and a ring that wraps drops the OLDEST - which
// means missing text in the middle of the answer, not just a late one.
constexpr size_t kEventRingMax = 8192;
}

UiServer::UiServer() : impl_(new Impl()) { impl_->owner = this; }

UiServer::~UiServer() {
  // Stop the rebind worker first: it calls back into this object.
  {
    std::lock_guard<std::mutex> lk(impl_->rebind_mu);
    impl_->rebind_quit = true;
    impl_->rebind_has_request = false;
  }
  impl_->rebind_cv.notify_all();
  if (impl_->rebind_worker.joinable()) impl_->rebind_worker.join();
  stop();
}

void UiServer::bind_api(FsApi* api) {
  impl_->bound_api = api;
  if (!api) return;
  impl_->api.reset(new UiApi(api));
  // collect events into the ring (the UI polls /api/events)
  Impl* im = impl_.get();
  impl_->listener_id = api->add_event_listener([im](const std::string& e, const Json& p) {
    {
      std::lock_guard<std::mutex> lk(im->mu);
      im->events.push_back(std::make_pair(im->next_event_id++,
                                          std::make_pair(e, p)));
      while (im->events.size() > kEventRingMax) im->events.pop_front();
    }
    // Wake every SSE writer immediately: a push, not a poll, is what makes a
    // streamed reply appear while the model is still generating.
    im->cv.notify_all();
  });
}

Json UiServer::drain_events(int64_t since, int64_t* next) {
  Json arr = Json::array();
  int64_t max_id = 0;
  std::lock_guard<std::mutex> lk(impl_->mu);
  // Bounded batch. A streaming reply can leave hundreds of events between two
  // polls; returning all of them makes one enormous JSON response that the UI
  // has to parse in one go. The cursor advances to the last id returned, so the
  // client simply polls again for the rest.
  static const size_t kMaxBatch = 128;
  for (size_t i = 0; i < impl_->events.size(); ++i) {
    const std::pair<int64_t, std::pair<std::string, Json> >& ev = impl_->events[i];
    if (ev.first <= since) continue;
    Json o = Json::object();
    o["id"] = ev.first;
    o["event"] = ev.second.first;
    o["payload"] = ev.second.second;
    arr.push_back(o);
    if (ev.first > max_id) max_id = ev.first;
    if (arr.size() >= kMaxBatch) break;
  }
  // Cursor must be the highest id actually returned. Using next_event_id here
  // would skip the very first event: an empty ring reports next=1, the client
  // then polls since=1 and never sees event id 1.
  if (next) *next = max_id > since ? max_id : since;
  return arr;
}

FsResult UiServer::start(int port, const std::string& host,
                         const std::string& password) {
  if (impl_->running.load()) return FsResult::success();
  impl_->stop_flag = std::make_shared<std::atomic<bool> >(false);
  impl_->host = host.empty() ? "127.0.0.1" : host;
  impl_->password = password;
  impl_->svr.reset(new httplib::Server());
  configure(*impl_->svr);

  int actual = 0;
  FsResult br = bind_server(*impl_->svr, port, &actual);
  if (!br.ok()) return br;
  impl_->port = actual;
  impl_->running.store(true);
  // Capture the SERVER by value. Capturing `this` and reading impl_->svr inside
  // the thread is a race: a rebind swaps impl_->svr, and the thread could then
  // call listen_after_bind() on the NEW instance (which already has its own
  // listen thread).
  httplib::Server* srv = impl_->svr.get();
  impl_->th = std::thread([srv]() { srv->listen_after_bind(); });
  // 等 listen 就绪（简单轮询）
  for (int i = 0; i < 100 && !impl_->svr->is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return FsResult::success();
}

// Reserve the socket without accepting yet. port 0 = let the OS pick.
FsResult UiServer::bind_server(httplib::Server& svr, int port, int* actual) {
  if (port > 0) {
    if (!svr.bind_to_port(impl_->host, port)) {
      return FsResult::error(ErrCode::kIo, "cannot bind " + impl_->host + ":" +
                                               std::to_string(port));
    }
    if (actual) *actual = port;
    return FsResult::success();
  }
  int p = svr.bind_to_any_port(impl_->host, 0);
  if (p <= 0) {
    return FsResult::error(ErrCode::kIo, "cannot bind " + impl_->host + ":0");
  }
  if (actual) *actual = p;
  return FsResult::success();
}

// All routes and the auth gate, registered on a freshly created Server. Split out
// of start() because httplib's Server cannot change its bind address once it has
// listened: a rebind creates a new instance and must re-register everything.
void UiServer::configure(httplib::Server& svr) {

  // ---- auth gate ----
  //
  // Only remote access is gated. Loopback is always allowed: the desktop app
  // itself talks to this server, and asking the local user for the password they
  // just set for their phone would be pointless friction.
  //
  // Remote clients get a token from POST /api/login (the raw password is never
  // stored client-side) and send it as `Authorization: Bearer <token>`.
  if (!impl_->password.empty()) {
    svr.set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res) {
      std::string addr = req.remote_addr;
      bool loopback = is_loopback(addr);
      if (loopback) return httplib::Server::HandlerResponse::Unhandled;
      // The login and auth-probe endpoints must be reachable without a token:
      // the page cannot show its password gate before it knows one is needed.
      //
      // The page SHELL is also public. It contains no data - just the empty
      // #fs-root, the stylesheets and the gate form - and it has to load before
      // the user can type a password. Everything that carries data lives under
      // /api/, which stays gated.
      if (req.path == "/api/login" || req.path == "/api/auth" ||
          req.path.rfind("/api/", 0) != 0) {
        return httplib::Server::HandlerResponse::Unhandled;
      }
      std::string auth = req.get_header_value("Authorization");
      const std::string prefix = "Bearer ";
      if (auth.size() > prefix.size() &&
          auth.compare(0, prefix.size(), prefix) == 0 &&
          impl_->valid_token_locked(auth.substr(prefix.size()))) {
        return httplib::Server::HandlerResponse::Unhandled;
      }      res.status = 401;
      res.set_header("WWW-Authenticate", "Bearer");
      res.set_content("{\"ok\":false,\"code\":\"unauthorized\","
                      "\"message\":\"password required\"}",
                      "application/json");
      return httplib::Server::HandlerResponse::Handled;
    });
  }

  // ---- backend API ----
  svr.Post("/api/call", [this](const httplib::Request& req, httplib::Response& res) {
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

  // ---- Server-Sent Events: the push channel ----
  //
  // The polling endpoint above still exists (tests and simple clients use it),
  // but the UI does NOT poll: browsers throttle background/hidden tabs to about
  // one timer tick per second, and the wasm bundle's setInterval(120ms) really
  // fired once a second. The UI then drained only one batch per second and fell
  // behind a fast model - the reply finished long before the last of it was
  // painted. A held-open response has no timer to throttle: the server writes
  // each event as it happens, so text appears while it is being generated.
  svr.Get("/api/stream", [this](const httplib::Request& req, httplib::Response& res) {
    int64_t since = 0;
    if (req.has_param("since")) {
      try {
        since = std::stoll(req.get_param_value("since"));
      } catch (...) {
        since = 0;
      }
    }
    Impl* im = impl_.get();
    std::shared_ptr<std::atomic<bool> > gen_stop = impl_->stop_flag;
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [im, gen_stop, since](size_t, httplib::DataSink& sink) mutable {
          // One wait per chunk. The timeout doubles as a keep-alive: a comment
          // line every second keeps idle proxies/browsers from deciding the
          // stream is dead; it also bounds how long stop() waits for this
          // handler to notice its generation is being retired.
          std::unique_lock<std::mutex> lk(im->mu);
          im->cv.wait_for(lk, std::chrono::milliseconds(500), [&] {
            if (gen_stop->load()) return true;
            for (size_t i = 0; i < im->events.size(); ++i) {
              if (im->events[i].first > since) return true;
            }
            return false;
          });
          // This listener is going away (a rebind is swapping it): end the
          // request now so stop() can join the worker thread. Returning false
          // stops the chunked stream, which the browser sees as a closed
          // connection - EventSource reconnects to the NEW listener on its own.
          // The new listener has a DIFFERENT flag, so it keeps serving.
          if (gen_stop->load()) return false;
          std::string out;
          size_t sent = 0;
          for (size_t i = 0; i < im->events.size() && sent < 256; ++i) {
            const std::pair<int64_t, std::pair<std::string, Json> >& ev = im->events[i];
            if (ev.first <= since) continue;
            Json o = Json::object();
            o["id"] = ev.first;
            o["event"] = ev.second.first;
            o["payload"] = ev.second.second;
            std::string line;
            try {
              line = o.dump();
            } catch (...) {
              line = "{\"event\":\"error\",\"payload\":{}}";
            }
            out += "data: " + line + "\n\n";
            since = ev.first;
            ++sent;
          }
          lk.unlock();
          if (out.empty()) out = ": keep-alive\n\n";
          return sink.write(out.data(), out.size());
        },
        [im](bool) { im->cv.notify_all(); });
  });

  // ---- auth probe: does THIS connection need a password? ----
  // The page calls it before mounting. Loopback answers false (the desktop app
  // and a browser on the same machine never see the gate), a remote connection
  // answers true and gets the password overlay until /api/login succeeds.
  svr.Get("/api/auth", [this](const httplib::Request& req,
                                      httplib::Response& res) {
    bool loopback = is_loopback(req.remote_addr);
    bool need = !impl_->password.empty() && !loopback;
    bool authed = !need;
    if (need) {
      std::string auth = req.get_header_value("Authorization");
      const std::string prefix = "Bearer ";
      authed = auth.size() > prefix.size() &&
               auth.compare(0, prefix.size(), prefix) == 0 &&
               impl_->valid_token_locked(auth.substr(prefix.size()));
    }
    res.set_header("Cache-Control", "no-store");
    res.set_content(Json{{"ok", true},
                         {"data", Json{{"required", need}, {"authed", authed}}}}
                        .dump(),
                    "application/json");
  });

  // ---- login: exchange the password for a session token ----
  svr.Post("/api/login", [this](const httplib::Request& req,
                                        httplib::Response& res) {
    if (impl_->password.empty()) {
      // No password configured: nothing to log in to.
      res.set_content("{\"ok\":true,\"data\":{\"token\":\"\"}}", "application/json");
      return;
    }
    Json body;
    try {
      body = Json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content("{\"ok\":false,\"code\":\"parse\",\"message\":\"bad json\"}",
                      "application/json");
      return;
    }
    std::string pw = jsonutil::get<std::string>(body, "password", "");
    if (!check_password(pw)) {
      res.status = 401;
      res.set_content("{\"ok\":false,\"code\":\"unauthorized\","
                      "\"message\":\"wrong password\"}",
                      "application/json");
      return;
    }
    std::string token = issue_token();
    res.set_content(Json{{"ok", true}, {"data", Json{{"token", token}}}}.dump(),
                    "application/json");
  });

  svr.Get("/api/events", [this](const httplib::Request& req, httplib::Response& res) {
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
  svr.Post("/api/upload", [this](const httplib::Request& req,
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
  svr.Get("/(.*)", [](const httplib::Request& req, httplib::Response& res) {
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
}

void UiServer::stop() {
  // Retire the CURRENT generation: make every open SSE stream end so the join
  // below cannot block (the connected page keeps a pool thread parked in its
  // stream handler otherwise). The flag is per generation, so a listener swapped
  // in by a rebind is unaffected.
  if (impl_ && impl_->stop_flag) impl_->stop_flag->store(true);
  if (impl_) impl_->cv.notify_all();
  if (impl_ && impl_->svr && impl_->running.load()) {
    impl_->svr->stop();
    if (impl_->th.joinable()) impl_->th.join();
    impl_->running.store(false);
  } else if (impl_ && impl_->th.joinable()) {
    // Defensive: a listen thread exists even if `running` was already cleared.
    impl_->th.join();
  }
  if (impl_ && impl_->bound_api && impl_->listener_id >= 0) {
    impl_->bound_api->remove_event_listener(impl_->listener_id);
    impl_->listener_id = -1;
  }
}

bool UiServer::running() const { return impl_ && impl_->running.load(); }

std::string UiServer::url() const {
  if (!impl_) return "";
  // 0.0.0.0 is a bind address, not a reachable one: print loopback for it so a
  // click-through still works, and let the settings page show the LAN URL.
  std::string h = (impl_->host == "0.0.0.0") ? "127.0.0.1" : impl_->host;
  return "http://" + h + ":" + std::to_string(impl_->port) + "/";
}

std::string UiServer::host() const { return impl_ ? impl_->host : ""; }
int UiServer::port() const { return impl_ ? impl_->port : 0; }

// Best-effort LAN address: open an unconnected UDP socket, "connect" it to a
// public address (no packet is sent - it only fixes the outbound route) and
// read back which local address the kernel chose. Works on Windows and POSIX
// with nothing but the socket headers httplib already pulls in, and unlike
// enumerating adapters it skips down/disconnected interfaces and VPN-less
// virtual ones, which is exactly the address the user can type on a phone.
std::string UiServer::lan_ip() const {
  socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) return std::string();
  struct sockaddr_in probe;
  std::memset(&probe, 0, sizeof(probe));
  probe.sin_family = AF_INET;
  probe.sin_port = htons(53);
  probe.sin_addr.s_addr = htonl(0x08080808u);   // 8.8.8.8
  std::string out;
  if (::connect(s, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
    struct sockaddr_in self;
    socklen_t len = static_cast<socklen_t>(sizeof(self));
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&self), &len) == 0 &&
        self.sin_family == AF_INET) {
      char buf[INET_ADDRSTRLEN] = {0};
      if (::inet_ntop(AF_INET, &self.sin_addr, buf, sizeof(buf))) out = buf;
    }
  }
  httplib::detail::close_socket(s);
  return out;
}

bool UiServer::requires_password() const {
  return impl_ && !impl_->password.empty();
}
bool UiServer::rebinding() const { return impl_ && impl_->rebinding.load(); }

std::string UiServer::last_error() const {
  if (!impl_) return std::string();
  std::lock_guard<std::mutex> lk(impl_->err_mu);
  return impl_->last_error;
}

bool UiServer::check_password(const std::string& password) const {
  if (impl_->password.empty()) return true;
  // Constant-time-ish compare: a length leak is acceptable here (the password is
  // local, not a remote account), but byte-by-byte early exit is not worth it.
  const std::string& a = impl_->password;
  if (a.size() != password.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ password[i]);
  }
  return diff == 0;
}

std::string UiServer::issue_token() {
  static std::random_device rd;
  static std::mt19937_64 gen(rd() ^ static_cast<uint64_t>(
                                         std::chrono::steady_clock::now()
                                             .time_since_epoch().count()));
  std::string token;
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) token += kHex[gen() & 0xF];
  std::lock_guard<std::mutex> lk(impl_->token_mu);
  impl_->tokens.insert(token);
  return token;
}

bool UiServer::valid_token(const std::string& token) const {
  if (token.empty()) return false;
  return impl_->valid_token_locked(token);
}

bool UiServer::is_loopback(const std::string& addr) {
  return addr == "127.0.0.1" || addr == "::1" || addr.rfind("127.", 0) == 0;
}

FsResult UiServer::restart(int port, const std::string& host,
                           const std::string& password) {
  // REBIND MUST NOT BLOCK THE CALLING REQUEST, and only ONE rebind may run.
  //
  // Two separate constraints, both learned the hard way:
  //   * Inline is impossible: restart() is reached from a settings.set handled by
  //     this very server, on one of its worker threads. stop() joins the listen
  //     thread, which joins the thread pool this request lives in -> deadlock.
  //   * One detached thread PER CALL is also wrong. Changing the host and then
  //     the port (or committing the port twice) gave two helpers that both called
  //     stop(); the second join() on an already-joined std::thread throws
  //     std::system_error, which is uncaught inside a detached thread ->
  //     std::terminate. The process vanished with no message.
  //
  // So there is ONE persistent worker. restart() only publishes the latest
  // desired settings and wakes it; if a rebind is already in flight the new
  // request replaces the pending one (last write wins - correct for a form).
  int old_port = impl_->port;
  int target_port = port > 0 ? port : old_port;
  bool port_changed = target_port > 0 && target_port != old_port;

  // Tell the page where we are going BEFORE dropping the socket it uses. Do this
  // here (not in the worker) because the worker may sleep and coalesce; the frame
  // has to leave the old listener while it is still serving. The page probes the
  // new port before navigating, so a later bind failure results in "still on the
  // old address", not a dead tab.
  if (port_changed) {
    Json payload = Json::object();
    payload["port"] = target_port;
    payload["host"] = host;
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->events.push_back(std::make_pair(impl_->next_event_id++,
                                           std::make_pair("server.moved", payload)));
    while (impl_->events.size() > kEventRingMax) impl_->events.pop_front();
    impl_->cv.notify_all();
  }

  impl_->rebinding.store(true);
  {
    std::lock_guard<std::mutex> lk(impl_->rebind_mu);
    impl_->rebind_port = target_port;
    impl_->rebind_host = host;
    impl_->rebind_password = password;
    impl_->rebind_has_request = true;
    // Start (or restart) the single worker. It serializes every rebind.
    if (!impl_->rebind_worker.joinable()) {
      Impl* im = impl_.get();
      impl_->rebind_worker = std::thread([im]() {
        for (;;) {
          int p = 0;
          std::string h, pw;
          {
            std::unique_lock<std::mutex> lk(im->rebind_mu);
            im->rebind_cv.wait(lk, [&] {
              return im->rebind_quit || im->rebind_has_request;
            });
            if (im->rebind_quit && !im->rebind_has_request) return;
            p = im->rebind_port;
            h = im->rebind_host;
            pw = im->rebind_password;
            im->rebind_has_request = false;
          }
          // Let the "server.moved" frame reach the browser (and the settings
          // response finish) before anything is torn down.
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          // The owner is UiServer; it outlives the worker because ~UiServer stops
          // and joins it (see ~UiServer).
          UiServer* owner = im->owner;
          owner->do_rebind(p, h, pw, p != 0 && p != im->port);
        }
      });
    }
  }
  impl_->rebind_cv.notify_all();
  return FsResult::success(Json{{"rebinding", true},
                                {"port", target_port},
                                {"moved", port_changed}});
}

// The swap itself. Runs on the single rebind worker (see restart), so it may
// block: stop() joins the listen thread and its pool.
void UiServer::do_rebind(int target_port, const std::string& host,
                         const std::string& password, bool /*port_changed*/) {
  // BIND THE NEW LISTENER FIRST, then drop the old one. This keeps the service
  // continuous (a stop-then-start leaves a window with nothing listening, and
  // the page's own requests fail in it) and proves the new address is free
  // before the old one is given up.
  std::unique_ptr<httplib::Server> next(new httplib::Server());
  std::string prev_host = impl_->host;
  std::string prev_pw = impl_->password;
  const int old_port = impl_->port;
  int actual = 0;
  // `configure` reads impl_->password, so set the new one before configuring.
  impl_->host = host;
  impl_->password = password;
  configure(*next);
  FsResult br = bind_server(*next, target_port, &actual);
  if (!br.ok()) {
    // Could not take the new address: put the old settings back and keep the
    // CURRENT listener running. Nothing is interrupted, and the settings page
    // reports the error.
    impl_->host = prev_host;
    impl_->password = prev_pw;
    {
      std::lock_guard<std::mutex> lk(impl_->err_mu);
      impl_->last_error = br.message;
    }
    impl_->rebinding.store(false);
    std::fprintf(stderr, "[ui] rebind to %s:%d failed: %s (kept %s:%d)\n",
                 host.c_str(), target_port, br.message.c_str(),
                 prev_host.c_str(), impl_->port);
    return;
  }

  // New socket is bound. Start accepting on the NEW listener BEFORE retiring the
  // old one: the page is told "server.moved" and needs the new port to answer
  // before it drops the old origin. A brief overlap is harmless (they are
  // different sockets) and it is what makes the navigation reliable.
  //
  // Capture the old generation's resources locally, put the new ones in place,
  // THEN close the old listener. Doing it in the other order (stop() first) left
  // a window with nothing listening, which is what made the page unable to do
  // anything after a port change.
  httplib::Server* old_svr = impl_->svr.release();
  std::unique_ptr<httplib::Server> old_owner(old_svr);
  std::thread old_th = std::move(impl_->th);
  std::shared_ptr<std::atomic<bool> > old_stop = impl_->stop_flag;
  bool port_moved = (actual != 0 && actual != old_port);

  impl_->svr = std::move(next);
  impl_->stop_flag = std::make_shared<std::atomic<bool> >(false);
  impl_->port = actual;
  impl_->running.store(true);
  // Capture the SERVER by value (see start()).
  httplib::Server* srv = impl_->svr.get();
  impl_->th = std::thread([srv]() { srv->listen_after_bind(); });
  for (int i = 0; i < 100 && !impl_->svr->is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Retire the previous listener now that the new one is serving. Its SSE
  // streams get its own flag and return; the new listener's streams are
  // untouched. Keep the API sink registered (it lives on Impl, not per server).
  //
  // When the PORT changed, keep the old socket alive a moment longer: the page
  // is in the middle of probing the new port and replacing its location, and
  // killing the old origin first is exactly what made a dead tab with nothing
  // left to click. The overlap only delays shutdown, never blocks a request.
  if (old_stop) old_stop->store(true);
  impl_->cv.notify_all();
  if (port_moved && old_owner) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  }
  if (old_owner) old_owner->stop();
  if (old_th.joinable()) old_th.join();

  {
    std::lock_guard<std::mutex> lk(impl_->err_mu);
    impl_->last_error.clear();
  }
  impl_->rebinding.store(false);
  std::fprintf(stderr, "[ui] server listening on %s:%d%s\n", host.c_str(),
               impl_->port, password.empty() ? "" : " (password required)");
}


}  // namespace ui
}  // namespace fs
