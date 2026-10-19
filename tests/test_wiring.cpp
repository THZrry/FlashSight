// test_wiring.cpp - the turn-addressed operations and the newly wired APIs.
//
// These cover the things that were "implemented" but unreachable: a turn needs a
// STABLE id before fork/rollback/forget can address it, and the UI needs the
// backend shapes it now relies on (command list, workflow runs, per-turn ids).
#include "core/fs_api.hpp"
#include "test_framework.hpp"

#include <string>

#include <ghc/filesystem.hpp>

using fs::Json;
using fs::jsonutil::get;

namespace {

class WClock : public fs::Clock {
 public:
  explicit WClock(fs::ClockMs start = 1000000) : now_(start) {}
  fs::ClockMs now_ms() const override { return now_; }
  void advance(fs::ClockMs d) { now_ += d; }

 private:
  fs::ClockMs now_;
};

class WRandom : public fs::Random {
 public:
  explicit WRandom(uint64_t seed) : state_(seed ? seed : 1) {}
  uint64_t next_u64() override {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return state_;
  }
  std::string hex(size_t nbytes) override {
    std::string out;
    const char* hexd = "0123456789abcdef";
    while (out.size() < nbytes * 2) {
      uint64_t v = next_u64();
      for (int i = 0; i < 16 && out.size() < nbytes * 2; ++i) {
        out.push_back(hexd[v & 0xF]);
        v >>= 4;
      }
    }
    return out;
  }

 private:
  uint64_t state_;
};

// A session with `n` turns, created through the public API (so append_turn runs
// and stamps whatever it is supposed to stamp).
struct Session {
  fstest::TempDir dir;
  WClock clock;
  WRandom rng;
  fs::FsApi api;
  std::string sid;

  explicit Session(const std::string& tag, int turns)
      : dir(tag), clock(1000), rng(7),
        api(ghc::filesystem::path(dir.path()), &clock, &rng) {
    ghc::filesystem::path proj = ghc::filesystem::path(dir.path()) / "p";
    ghc::filesystem::create_directories(proj);
    std::string pid = api.project_create(proj.string()).data["pid"].get<std::string>();
    sid = api.session_new(pid, proj.string()).data["sid"].get<std::string>();
    for (int i = 0; i < turns; ++i) {
      Json t = Json::object();
      t["role"] = (i % 2 == 0) ? "user" : "assistant";
      t["text"] = "turn " + std::to_string(i);
      api.session_append_turn(sid, t);
      clock.advance(1000);
    }
  }
};

}  // namespace

// Every appended turn must get a turnId. Without it fork/rollback/forget can
// never find the turn they were asked for - which is exactly what happened: the
// three APIs existed but always answered "turn not found".
FS_TEST(wiring_append_turn_stamps_turn_id) {
  Session s("wire_tid", 3);
  fs::FsResult r = s.api.session_get(s.sid);
  ASSERT_TRUE(r.ok());
  const Json& turns = r.data["turns"];
  ASSERT_TRUE(turns.is_array() && turns.size() == 3);
  std::string a = get<std::string>(turns[0], "turnId", "");
  std::string b = get<std::string>(turns[1], "turnId", "");
  std::string c = get<std::string>(turns[2], "turnId", "");
  EXPECT_FALSE(a.empty());
  EXPECT_FALSE(b.empty());
  EXPECT_FALSE(c.empty());
  EXPECT_TRUE(a != b);
  EXPECT_TRUE(b != c);
  // The id is addressable: rollback finds it.
  EXPECT_TRUE(s.api.session_rollback(s.sid, b).ok());
  fs::FsResult after = s.api.session_get(s.sid);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.data["turns"].size(), size_t(1));  // rollback truncates AT b
}

// Fork keeps the turns up to and including the chosen one, under a NEW session.
FS_TEST(wiring_fork_from_turn) {
  Session s("wire_fork", 4);
  fs::FsResult full = s.api.session_get(s.sid);
  ASSERT_TRUE(full.ok());
  std::string tid = get<std::string>(full.data["turns"][1], "turnId", "");
  ASSERT_TRUE(!tid.empty());
  fs::FsResult fk = s.api.session_fork(s.sid, tid);
  ASSERT_TRUE(fk.ok());
  std::string new_sid = get<std::string>(fk.data, "sid", "");
  EXPECT_FALSE(new_sid.empty());
  EXPECT_TRUE(new_sid != s.sid);
  fs::FsResult branched = s.api.session_get(new_sid);
  ASSERT_TRUE(branched.ok());
  // turns 0..1 => two turns
  EXPECT_EQ(branched.data["turns"].size(), size_t(2));
  EXPECT_EQ(get<std::string>(branched.data["turns"][1], "text", ""),
            std::string("turn 1"));
  // the original is untouched
  EXPECT_EQ(s.api.session_get(s.sid).data["turns"].size(), size_t(4));
}

// Forget is informational: the turn stays on disk but is marked, so history
// building can skip it.
FS_TEST(wiring_forget_turn_marks_it) {
  Session s("wire_forget", 3);
  fs::FsResult full = s.api.session_get(s.sid);
  std::string tid = get<std::string>(full.data["turns"][1], "turnId", "");
  ASSERT_TRUE(!tid.empty());
  EXPECT_TRUE(s.api.session_forget(s.sid, tid).ok());
  fs::FsResult after = s.api.session_get(s.sid);
  EXPECT_EQ(after.data["turns"].size(), size_t(3));  // still there
  EXPECT_TRUE(get<bool>(after.data["turns"][1], "forgotten", false));
  EXPECT_FALSE(get<bool>(after.data["turns"][0], "forgotten", false));
  // an unknown id is still an error
  EXPECT_TRUE(s.api.session_forget(s.sid, "t_nope").code == fs::ErrCode::kNotFound);
}

// The composer's command menu is a BACKEND list: built-ins plus the user-callable
// skills, and never the built-in MCP entries (they have no "/name" form).
FS_TEST(wiring_command_list_has_builtins_and_no_mcp) {
  Session s("wire_cmd", 0);
  fs::FsResult r = s.api.command_list(s.sid);
  ASSERT_TRUE(r.ok());
  const Json& cmds = r.data["commands"];
  ASSERT_TRUE(cmds.is_array());
  bool plan = false, suspend = false, browser = false, lsp = false;
  for (size_t i = 0; i < cmds.size(); ++i) {
    std::string name = get<std::string>(cmds[i], "name", "");
    if (name == "plan") {
      plan = true;
      EXPECT_EQ(get<std::string>(cmds[i], "action", ""), std::string("ui:plan"));
    }
    if (name == "suspend") {
      suspend = true;
      EXPECT_EQ(get<std::string>(cmds[i], "action", ""),
                std::string("session.suspend"));
    }
    if (name == "browser-control") browser = true;
    if (name == "lsp") lsp = true;
  }
  EXPECT_TRUE(plan);
  EXPECT_TRUE(suspend);
  EXPECT_FALSE(browser);  // built-in MCP: model-invoked only
  EXPECT_FALSE(lsp);
}

// workflow.runs is what the panel lists; it must be scoped to the session.
FS_TEST(wiring_workflow_runs_scoped) {
  Session s("wire_wf", 0);
  fs::FsResult defs = s.api.workflow_list();
  ASSERT_TRUE(defs.ok());
  EXPECT_TRUE(defs.data["defs"].is_array());
  fs::FsResult runs = s.api.workflow_runs(s.sid);
  ASSERT_TRUE(runs.ok());
  EXPECT_TRUE(runs.data["runs"].is_array());
  // a session with no runs is empty, not an error
  EXPECT_EQ(runs.data["runs"].size(), size_t(0));
}

// Per-session usage exposes the two numbers the context water level needs, and
// they are the LAST request's prompt tokens (not the session total).
FS_TEST(wiring_usage_reports_last_request) {
  Session s("wire_usage", 0);
  fs::UsageRow a;
  a.ts = 1000;
  a.sid = s.sid;
  a.provider = "p";
  a.model = "m";
  a.in = 100;
  a.out = 10;
  a.cost_kind = "money";
  EXPECT_TRUE(s.api.usage().record(a).ok());
  fs::UsageRow b;
  b.ts = 2000;
  b.sid = s.sid;
  b.provider = "p";
  b.model = "m";
  b.in = 5000;
  b.out = 20;
  b.cost_kind = "money";
  EXPECT_TRUE(s.api.usage().record(b).ok());
  fs::FsResult q = s.api.usage_query(Json{{"sid", s.sid}});
  ASSERT_TRUE(q.ok());
  EXPECT_EQ(get<int64_t>(q.data, "totalIn", 0), int64_t(5100));
  EXPECT_EQ(get<int64_t>(q.data, "lastIn", 0), int64_t(5000));  // not 5100
  EXPECT_EQ(get<std::string>(q.data, "lastModel", ""), std::string("m"));
}

// Export with no path writes into the data root and reports where: an empty path
// used to mean "wherever the process was started".
FS_TEST(wiring_usage_export_defaults_to_data_root) {
  Session s("wire_csv", 0);
  fs::UsageRow a;
  a.ts = 1000;
  a.sid = s.sid;
  a.provider = "p";
  a.model = "m";
  a.in = 1;
  s.api.usage().record(a);
  fs::FsResult r = s.api.usage_export_csv(Json{{"sid", s.sid}}, "");
  ASSERT_TRUE(r.ok());
  std::string path = get<std::string>(r.data, "path", "");
  EXPECT_FALSE(path.empty());
  EXPECT_TRUE(ghc::filesystem::exists(path));
  EXPECT_TRUE(path.find("exports") != std::string::npos);
}
