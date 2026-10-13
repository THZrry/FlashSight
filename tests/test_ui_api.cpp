// test_ui_api.cpp - UiApi dispatch + NativeBackend end-to-end against a real
// FsApi in a temp root. No network: project/session/settings/terminal only.
#include "test_framework.hpp"

#include "core/fs_api.hpp"
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
