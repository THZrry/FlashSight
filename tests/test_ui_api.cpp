// test_ui_api.cpp - UiApi dispatch + NativeBackend end-to-end against a real
// FsApi in a temp root. No network: project/session/settings/terminal only.
#include "test_framework.hpp"

#include "core/fs_api.hpp"
#include "core/settings_store.hpp"
#include "ui/ui_api.hpp"
#include "ui/ui_backend.hpp"
#include "ui/ui_native_backend.hpp"

using namespace fs;
using namespace fs::ui;

namespace {

Json call_api(FsApi& api, const std::string& method, const Json& args) {
  UiApi u(&api);
  FsResult r = u.call(method, args);
  Json out = Json::object();
  out["ok"] = r.ok();
  out["data"] = r.data;
  out["code"] = err_code_name(r.code);
  out["message"] = r.message;
  return out;
}

}  // namespace

FS_TEST(uiapi_unknown_method) {
  fstest::TempDir tmp("uiapi");
  FsApi api(tmp.path());
  UiApi u(&api);
  EXPECT_FALSE(u.has("nope.nothing"));
  FsResult r = u.call("nope.nothing", Json::object());
  EXPECT_FALSE(r.ok());
  EXPECT_TRUE(r.code == ErrCode::kNotFound);
}

FS_TEST(uiapi_project_and_session_flow) {
  fstest::TempDir tmp("uiapi2");
  FsApi api(tmp.path());
  UiApi u(&api);

  // create a project dir and register it
  std::string proj = tmp.path() + "/proj";
  ghc::filesystem::create_directories(proj);
  FsResult pc = u.call("project.create", Json{{"path", proj}});
  ASSERT_TRUE(pc.ok());
  std::string pid = jsonutil::get<std::string>(pc.data, "pid", "");
  EXPECT_TRUE(!pid.empty());

  FsResult pl = u.call("project.list", Json::object());
  ASSERT_TRUE(pl.ok());
  EXPECT_TRUE(pl.data.is_array());
  EXPECT_TRUE(pl.data.size() >= 1);

  // new session
  FsResult sn = u.call("session.new", Json{{"pid", pid}, {"cwd", proj}});
  ASSERT_TRUE(sn.ok());
  std::string sid = jsonutil::get<std::string>(sn.data, "sid", "");
  EXPECT_TRUE(!sid.empty());

  FsResult sl = u.call("session.list", Json{{"pid", pid}});
  ASSERT_TRUE(sl.ok());
  EXPECT_TRUE(sl.data.is_array());
  EXPECT_TRUE(sl.data.size() >= 1);

  FsResult sg = u.call("session.get", Json{{"sid", sid}});
  ASSERT_TRUE(sg.ok());

  // settings round-trip
  FsResult ss = u.call("settings.set", Json{{"key", "history.recentTurns"}, {"value", 30}});
  ASSERT_TRUE(ss.ok());
  FsResult sget = u.call("settings.get", Json{{"key", "history.recentTurns"}});
  ASSERT_TRUE(sget.ok());

  // thinking language (writes global settings + host)
  FsResult tl = u.call("thinking.setLang", Json{{"lang", "en"}});
  ASSERT_TRUE(tl.ok());
}

FS_TEST(uiapi_usage_query) {
  fstest::TempDir tmp("uiapi3");
  FsApi api(tmp.path());
  UiApi u(&api);
  FsResult r = u.call("usage.query", Json{{"filter", Json::object()}});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r.data.contains("calls"));
  EXPECT_TRUE(r.data.contains("byProviderModel"));
}

FS_TEST(uiapi_skill_and_shells) {
  fstest::TempDir tmp("uiapi4");
  FsApi api(tmp.path());
  UiApi u(&api);
  FsResult sk = u.call("skill.list", Json::object());
  ASSERT_TRUE(sk.ok());
  FsResult sh = u.call("settings.detectShells", Json::object());
  ASSERT_TRUE(sh.ok());
}

FS_TEST(native_backend_call_and_result) {
  fstest::TempDir tmp("natbe");
  FsApi api(tmp.path());
  NativeBackend be(&api);
  // a successful call
  BackendResult ok = be.call("usage.query", Json{{"filter", Json::object()}});
  EXPECT_TRUE(ok.ok);
  EXPECT_TRUE(ok.data.contains("calls"));
  // a failing call carries a machine-readable code
  BackendResult bad = be.call("nope.nothing", Json::object());
  EXPECT_FALSE(bad.ok);
  EXPECT_EQ(bad.code, std::string("not-found"));
}

FS_TEST(native_backend_receives_events) {
  fstest::TempDir tmp("natbe2");
  FsApi api(tmp.path());
  NativeBackend be(&api);
  std::vector<std::string> got;
  be.subscribe([&got](const std::string& e, const Json&) { got.push_back(e); });
  // session.send on a fresh session emits loop/turn events (echo mode: no provider)
  FsResult pc = api.project_create(tmp.path() + "/p");
  ghc::filesystem::create_directories(tmp.path() + "/p");
  FsResult sn = api.session_new(jsonutil::get<std::string>(pc.data, "pid", ""), tmp.path() + "/p");
  std::string sid = jsonutil::get<std::string>(sn.data, "sid", "");
  ASSERT_TRUE(!sid.empty());
  api.session_send(sid, "hi", Json::array());
  be.poll();
  EXPECT_TRUE(!got.empty());  // at least one event was delivered
}

namespace {

struct TermSession {
  fstest::TempDir tmp;
  FsApi api;
  std::string sid;
  explicit TermSession(const char* tag) : tmp(tag), api(tmp.path()) {
    std::string dir = tmp.path() + "/p";
    ghc::filesystem::create_directories(dir);
    FsResult pc = api.project_create(dir);
    FsResult sn = api.session_new(jsonutil::get<std::string>(pc.data, "pid", ""), dir);
    sid = jsonutil::get<std::string>(sn.data, "sid", "");
  }
};

}  // namespace

// The terminal event is the ONLY bridge between the process registry and the
// drawer. FsApi rebuilt the payload with {sid,tid,text} and dropped kind/label/
// timedOut, so the UI classified every model terminal as an unknown ghost while
// the backend had a real record.
FS_TEST(uiapi_terminal_output_event_carries_kind) {
  TermSession ts("term_evt");
  ASSERT_TRUE(!ts.sid.empty());
  std::vector<std::string> kinds;
  ts.api.set_event_sink([&kinds](const std::string& e, const Json& p) {
    if (e == "terminal.output") {
      kinds.push_back(jsonutil::get<std::string>(p, "kind", ""));
    }
  });
  Json out;
  Json args = Json{{"sid", ts.sid},
                   {"command", "echo term-kind"},
                   {"background", false},
                   {"timeoutMs", 5000}};
  ASSERT_TRUE(ts.api.spawn_terminal(args, &out).ok());
  ts.api.terminal_tick();
  bool saw_fg = false;
  for (size_t i = 0; i < kinds.size(); ++i) {
    if (kinds[i] == "model-fg") saw_fg = true;
  }
  EXPECT_TRUE(saw_fg);
}

// The foreground budget is enforced by the tool: at the deadline the call
// returns `timedOut` with the command STILL RUNNING, and the record is a real
// background terminal that terminal.list must return (so the drawer can open
// it). This is the UI/backend connection the user could not find.
FS_TEST(uiapi_terminal_timeout_is_listed_and_running) {
  TermSession ts("term_timeout");
  ASSERT_TRUE(!ts.sid.empty());
  // Portable long-ish command (BusyBox ping has no -n count and would exit
  // immediately with a usage error).
  std::string cmd = "ping 127.0.0.1";
  Json out;
  Json args = Json{{"sid", ts.sid},
                   {"command", cmd},
                   {"background", false},
                   {"timeoutMs", 300}};
  FsResult r = ts.api.spawn_terminal(args, &out);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(jsonutil::get<bool>(out, "timedOut", false));
  EXPECT_EQ(jsonutil::get<std::string>(out, "status", ""), std::string("running"));
  std::string tid = jsonutil::get<std::string>(out, "tid", "");
  ASSERT_TRUE(!tid.empty());
  FsResult list = ts.api.terminal_list(ts.sid);
  ASSERT_TRUE(list.ok());
  ASSERT_TRUE(list.data.is_array());
  bool found = false;
  for (size_t i = 0; i < list.data.size(); ++i) {
    if (jsonutil::get<std::string>(list.data[i], "tid", "") != tid) continue;
    found = true;
    EXPECT_EQ(jsonutil::get<std::string>(list.data[i], "kind", ""),
              std::string("model-bg"));
    EXPECT_TRUE(jsonutil::get<bool>(list.data[i], "timedOut", false));
    EXPECT_EQ(jsonutil::get<std::string>(list.data[i], "status", ""),
              std::string("running"));
  }
  EXPECT_TRUE(found);
  ASSERT_TRUE(ts.api.terminal_kill(tid).ok());
}

// Native builds must START a server for 0.0.0.0 but never for 127.0.0.1; the
// Web build allows both. Switching 0.0.0.0 -> 127.0.0.1 in a native build stops
// the server instead of leaving a loopback listener running.
FS_TEST(uiapi_server_host_rules_by_variant) {
  fstest::TempDir tmp("srvhost");
  FsApi api(tmp.path());
  int restarts = 0, stops = 0;
  std::string last_host;
  Json status = Json{{"running", false}, {"host", ""}, {"port", 0}};
  api.attach_ui_server(
      [&](int, const std::string& host, const std::string&) {
        ++restarts;
        last_host = host;
        status["running"] = true;
        status["host"] = host;
        return true;
      },
      [&]() { return status; },
      [&]() {
        ++stops;
        status["running"] = false;
      },
      false);
  api.settings_set(kKeyServerHost, Json("0.0.0.0"), "");
  EXPECT_EQ(restarts, 1);
  EXPECT_EQ(last_host, std::string("0.0.0.0"));
  EXPECT_TRUE(jsonutil::get<bool>(api.server_status().data, "localAllowed", true) == false);
  api.settings_set(kKeyServerHost, Json("127.0.0.1"), "");
  EXPECT_EQ(stops, 1);
  EXPECT_EQ(restarts, 1);
}

FS_TEST(uiapi_server_loopback_allowed_for_web) {
  fstest::TempDir tmp("srvweb");
  FsApi api(tmp.path());
  int restarts = 0;
  Json status = Json{{"running", false}};
  api.attach_ui_server(
      [&](int, const std::string&, const std::string&) {
        ++restarts;
        status["running"] = true;
        return true;
      },
      [&]() { return status; }, [&]() { status["running"] = false; }, true);
  api.settings_set(kKeyServerHost, Json("127.0.0.1"), "");
  EXPECT_EQ(restarts, 1);
  EXPECT_TRUE(jsonutil::get<bool>(api.server_status().data, "localAllowed", false));
}
