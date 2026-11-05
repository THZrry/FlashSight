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
  EXPECT_FALSE(plan);  // the plan is a document now; no panel command exists
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

// Session history is SHARDED: turns live in turns/NNN.json and full.json keeps
// only the small head. Without this every append re-wrote the whole history
// (O(n) per turn, one file growing without bound - a real session reached 90 KB
// in six turns because tool results are stored in full).
// The crash-safe live snapshot survives a restart.
//
// A turn is only appended when it finishes, so a crashed process used to lose the
// whole answer. The snapshot lives next to the session (runtime.json's `live`),
// which means a NEW FsApi over the same directory must find it.
FS_TEST(wiring_live_snapshot_survives_restart) {
  fstest::TempDir dir("wire_live");
  WClock clock(1000);
  WRandom rng(7);
  std::string sid;
  ghc::filesystem::path proj = ghc::filesystem::path(dir.path()) / "p";
  {
    fs::FsApi api(ghc::filesystem::path(dir.path()), &clock, &rng);
    ghc::filesystem::create_directories(proj);
    std::string pid = api.project_create(proj.string()).data["pid"].get<std::string>();
    sid = api.session_new(pid, proj.string()).data["sid"].get<std::string>();
    // While a turn runs the host writes a snapshot every couple of seconds.
    api.sessions().write_live_snapshot(
        sid, Json{{"running", true}, {"text", "half an answer"},
                  {"userText", "the question"}, {"startedAt", 500}});
  }
  // New process (same data dir): the in-memory loop is gone, but the snapshot is
  // on disk - and that is what the UI must be handed.
  {
    fs::FsApi api2(ghc::filesystem::path(dir.path()), &clock, &rng);
    fs::FsResult r = api2.session_live_turn(sid);
    ASSERT_TRUE(r.ok());
    bool running = get<bool>(r.data, "running", true);
    bool crashed = get<bool>(r.data, "crashed", false);
    EXPECT_FALSE(running);   // not running now
    EXPECT_TRUE(crashed);    // recovered crash
    EXPECT_EQ(get<std::string>(r.data, "text", ""), std::string("half an answer"));
    EXPECT_EQ(get<std::string>(r.data, "userText", ""), std::string("the question"));
  }
}

FS_TEST(wiring_history_is_sharded_and_readable) {
  Session s("wire_shard", 0);
  // Enough turns to cross a shard boundary (16 per shard).
  for (int i = 0; i < 20; ++i) {
    Json t = Json::object();
    t["role"] = "user";
    t["text"] = "turn " + std::to_string(i);
    s.api.session_append_turn(s.sid, t);
  }
  fs::FsResult r = s.api.session_get(s.sid);
  ASSERT_TRUE(r.ok());
  // Every turn still reads back, in order, through the same API as before.
  ASSERT_TRUE(r.data["turns"].is_array());
  EXPECT_EQ(r.data["turns"].size(), size_t(20));
  EXPECT_EQ(get<std::string>(r.data["turns"][0], "text", ""), std::string("turn 0"));
  EXPECT_EQ(get<std::string>(r.data["turns"][19], "text", ""), std::string("turn 19"));

  // The layout on disk: shards 000 and 001 exist, and the head file itself no
  // longer carries the turns.
  ghc::filesystem::path sdir = ghc::filesystem::path(s.dir.path()) / "p" /
                              ".flashsight" / "sessions" / s.sid;
  EXPECT_TRUE(ghc::filesystem::exists(sdir / "turns" / "000.json"));
  EXPECT_TRUE(ghc::filesystem::exists(sdir / "turns" / "001.json"));
  {
    std::ifstream ifs((sdir / "full.json").string(), std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    Json head = Json::parse(ss.str());
    EXPECT_FALSE(head.contains("turns"));  // sharded away
    EXPECT_TRUE(head.contains("meta"));
  }
  // A shard holds at most 16 turns.
  {
    std::ifstream ifs((sdir / "turns" / "000.json").string(), std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    Json part = Json::parse(ss.str());
    EXPECT_EQ(part.size(), size_t(16));
  }

  // Rollback shortens the history: the stale second shard must be gone, or the
  // dropped turns would come back on the next read.
  std::string tid = get<std::string>(r.data["turns"][4], "turnId", "");
  ASSERT_TRUE(!tid.empty());
  EXPECT_TRUE(s.api.session_rollback(s.sid, tid).ok());
  fs::FsResult after = s.api.session_get(s.sid);
  EXPECT_EQ(after.data["turns"].size(), size_t(4));
  EXPECT_FALSE(ghc::filesystem::exists(sdir / "turns" / "001.json"));
}

// A subagent's record must survive a restart.
//
// The record used to live ONLY in the in-memory SubagentManager, so after the
// process restarted every "打开记录" button in an old transcript pointed at
// nothing ("子代理还是无法在历史记录里打开（也有可能是旧的历史记录没存）").
FS_TEST(wiring_subagent_record_survives_restart) {
  fstest::TempDir dir("wire_sub");
  WClock clock(1000);
  WRandom rng(7);
  ghc::filesystem::path proj = ghc::filesystem::path(dir.path()) / "p";
  std::string sid;
  {
    fs::FsApi api(ghc::filesystem::path(dir.path()), &clock, &rng);
    ghc::filesystem::create_directories(proj);
    std::string pid = api.project_create(proj.string()).data["pid"].get<std::string>();
    sid = api.session_new(pid, proj.string()).data["sid"].get<std::string>();
    Json rec = Json::object();
    rec["id"] = "sub_1";
    rec["kind"] = "research";
    rec["status"] = "done";
    rec["task"] = "look at the parser";
    rec["summary"] = "it parses";
    rec["full"] = Json{{"transcript", Json::array({Json{{"role", "assistant"},
                                                        {"content", "done"}}})}};
    api.sessions().write_subagent_record(sid, "sub_1", rec);
  }
  // New process: nothing in memory, but the record is on disk next to the session.
  {
    fs::FsApi api2(ghc::filesystem::path(dir.path()), &clock, &rng);
    fs::FsResult r = api2.subagent_detail_in(sid, "sub_1");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(get<std::string>(r.data, "task", ""), std::string("look at the parser"));
    EXPECT_EQ(get<std::string>(r.data, "summary", ""), std::string("it parses"));
    ASSERT_TRUE(r.data.contains("full"));
    EXPECT_EQ(r.data["full"]["transcript"].size(), size_t(1));
    // The file lives where a reader expects it, one per record.
    ghc::filesystem::path rec = proj / ".flashsight" / "sessions" / sid /
                                "subagents" / "sub_1.json";
    EXPECT_TRUE(ghc::filesystem::exists(rec));
  }
}

// A path separator in the id must not escape the session directory (the id is
// generated internally, but it is still treated as untrusted).
FS_TEST(wiring_subagent_record_id_is_sanitised) {
  fstest::TempDir dir("wire_sub2");
  WClock clock(1000);
  WRandom rng(7);
  fs::FsApi api(ghc::filesystem::path(dir.path()), &clock, &rng);
  ghc::filesystem::path proj = ghc::filesystem::path(dir.path()) / "p";
  ghc::filesystem::create_directories(proj);
  std::string pid = api.project_create(proj.string()).data["pid"].get<std::string>();
  std::string sid = api.session_new(pid, proj.string()).data["sid"].get<std::string>();
  Json rec = Json::object();
  rec["id"] = "../evil";
  rec["task"] = "x";
  api.sessions().write_subagent_record(sid, "../evil", rec);
  ghc::filesystem::path sdir = proj / ".flashsight" / "sessions" / sid;
  EXPECT_FALSE(ghc::filesystem::exists(proj / ".flashsight" / "sessions" / "evil.json"));
  EXPECT_FALSE(ghc::filesystem::exists(proj / ".flashsight" / "evil.json"));
  // ".", ".", "/" each map to "_", so the record lands as ___evil.json INSIDE the
  // session's own subagents/ directory rather than escaping it.
  EXPECT_TRUE(ghc::filesystem::exists(sdir / "subagents" / "___evil.json"));
}

// A session written by an OLDER build (turns inline in full.json, no turns/
// directory) must keep working: same read API, and the next write migrates it to
// the sharded layout without losing a turn.
FS_TEST(wiring_legacy_full_json_still_reads) {
  fstest::TempDir dir("wire_legacy");
  WClock clock(1000);
  WRandom rng(7);
  fs::FsApi api(ghc::filesystem::path(dir.path()), &clock, &rng);
  ghc::filesystem::path proj = ghc::filesystem::path(dir.path()) / "p";
  ghc::filesystem::create_directories(proj);
  std::string pid = api.project_create(proj.string()).data["pid"].get<std::string>();
  std::string sid = api.session_new(pid, proj.string()).data["sid"].get<std::string>();

  // Hand-write the OLD format: turns inline, no turns/ dir.
  ghc::filesystem::path sdir = proj / ".flashsight" / "sessions" / sid;
  Json old = Json::object();
  old["meta"] = Json{{"pid", pid}, {"cwd", proj.string()}, {"title", "legacy"}};
  old["sessionSettings"] = fs::default_session_settings(Json::object());
  old["snapshots"] = Json::array();
  old["systemOps"] = Json::array();
  Json turns = Json::array();
  for (int i = 0; i < 5; ++i) {
    turns.push_back(Json{{"role", "user"}, {"text", "old " + std::to_string(i)},
                         {"turnId", "t_old" + std::to_string(i)}, {"ts", 500 + i}});
  }
  old["turns"] = turns;
  {
    std::ofstream ofs((sdir / "full.json").string(), std::ios::binary | std::ios::trunc);
    ofs << old.dump();
  }

  // Reads back unchanged, through the normal API.
  fs::FsResult r = api.session_get(sid);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r.data["turns"].is_array());
  EXPECT_EQ(r.data["turns"].size(), size_t(5));
  EXPECT_EQ(get<std::string>(r.data["turns"][4], "text", ""), std::string("old 4"));

  // Append: the migration hands the old turns to shards and keeps order.
  clock.advance(1000);
  api.session_append_turn(sid, Json{{"role", "assistant"}, {"text", "new one"}});
  fs::FsResult after = api.session_get(sid);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.data["turns"].size(), size_t(6));
  EXPECT_EQ(get<std::string>(after.data["turns"][0], "text", ""), std::string("old 0"));
  EXPECT_EQ(get<std::string>(after.data["turns"][5], "text", ""), std::string("new one"));
}


// ---------- 新项目自带一个会话 ----------
// The sidebar lists SESSIONS under projects, so a project without one appeared on
// the home page and nowhere else ("新打开的项目会出现在主页，但不会出现在侧边栏").
FS_TEST(project_open_creates_a_first_session) {
  using namespace fs;
  fstest::TempDir td("projsess");
  FsApi api(ghc::filesystem::path(td.path()));
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "fresh";
  ghc::filesystem::create_directories(proj_dir);

  FsResult pr = api.project_open(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sl = api.session_list(pid);
  ASSERT_TRUE(sl.ok());
  ASSERT_TRUE(sl.data.is_array());
  EXPECT_TRUE(sl.data.size() == 1);

  // Opening the same folder again must NOT add another one.
  ASSERT_TRUE(api.project_open(proj_dir.string()).ok());
  FsResult sl2 = api.session_list(pid);
  ASSERT_TRUE(sl2.ok());
  EXPECT_TRUE(sl2.data.size() == 1);
}

// ---------- 模型的命令必须在项目根跑 ----------
// The model's terminal tools used to call TerminalManager::spawn directly, which
// skipped the cwd default: `git log` then ran in the APP's directory (the release
// folder, inside this repo) and the model read FlashSight's own history instead of
// the user's project.
FS_TEST(terminal_spawn_defaults_to_the_project_root) {
  using namespace fs;
  fstest::TempDir td("termcwd");
  FsApi api(ghc::filesystem::path(td.path()));
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  // No cwd passed, exactly like the tool call the model makes.
  FsResult sr = api.session_new(pid, "");
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  Json args = Json::object();
  args["sid"] = sid;
  args["command"] = "echo cwd-probe";
  args["background"] = false;
  args["timeoutMs"] = 15000;
  // A label marks the terminal as worth keeping; an unlabelled quick model
  // command is deliberately pruned from the list (batch 54).
  args["label"] = "cwd-probe";
  Json out;
  FsResult r = api.spawn_terminal(args, &out);
  ASSERT_TRUE(r.ok());
  std::string tid = jsonutil::get<std::string>(out, "tid", "");
  EXPECT_TRUE(!tid.empty());

  FsResult list = api.terminal_list(sid);
  ASSERT_TRUE(list.ok());
  ASSERT_TRUE(list.data.is_array() && list.data.size() >= 1);
  std::string cwd = jsonutil::get<std::string>(list.data[0], "cwd", "");
  EXPECT_TRUE(!cwd.empty());
  // It must be the session's project, not wherever the app was started from.
  std::string tail = cwd.substr(cwd.size() >= 4 ? cwd.size() - 4 : 0);
  EXPECT_EQ(tail, std::string("proj"));

  // An UNLABELLED quick foreground command is pruned: the list stays at one
  // record (the marked probe above). It is tool noise, not a transcript.
  Json quick = Json::object();
  quick["sid"] = sid;
  quick["command"] = "echo quick";
  quick["background"] = false;
  quick["timeoutMs"] = 15000;
  Json qout;
  FsResult qr = api.spawn_terminal(quick, &qout);
  ASSERT_TRUE(qr.ok());
  FsResult list2 = api.terminal_list(sid);
  ASSERT_TRUE(list2.ok());
  ASSERT_TRUE(list2.data.is_array());
  EXPECT_EQ((int)list2.data.size(), 1);
}

// ---------- 终端的记录必须活过重启 ----------
// A terminal process cannot survive the app, but "which terminals did I open and
// what did they print" must: the session's terminals/ directory was created and
// then never written, so every restart showed an empty list (the user checked the
// folder). The list merges saved entries (listed as exited) and the grid falls
// back to the saved log.
FS_TEST(terminal_record_survives_restart) {
  using namespace fs;
  fstest::TempDir td("termrec");
  FsApi api(ghc::filesystem::path(td.path()));
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  // An empty session lists no terminals.
  FsResult before = api.terminal_list(sid);
  ASSERT_TRUE(before.ok());
  EXPECT_TRUE(before.data.is_array());
  EXPECT_EQ((int)before.data.size(), 0);

  // A "previous run" left a record: metadata + the PTY log (with escape codes).
  ghc::filesystem::path tdir =
      ghc::filesystem::path(proj_dir) / ".flashsight" / "sessions" / sid / "terminals";
  ghc::filesystem::create_directories(tdir);
  Json meta = Json{{"tid", "t_1"}, {"sid", sid}, {"shell", "bash"},
                   {"status", "exited"}, {"startedAt", 1000}, {"exitCode", 0},
                   {"logPath", (tdir / "t_1.log").string()}};
  {
    std::ofstream ofs((tdir / "t_1.json").string(), std::ios::binary);
    ofs << meta.dump(2);
  }
  {
    std::ofstream ofs((tdir / "t_1.log").string(), std::ios::binary);
    ofs << "\x1b[32mhello\x1b[0m\r\nworld\r\n";
  }

  FsResult after = api.terminal_list(sid);
  ASSERT_TRUE(after.ok());
  ASSERT_TRUE(after.data.size() == 1);
  EXPECT_EQ(after.data[0]["tid"].get<std::string>(), std::string("t_1"));
  // Its process is gone, so the entry is reported as exited no matter what the
  // file says.
  EXPECT_EQ(after.data[0]["status"].get<std::string>(), std::string("exited"));
  EXPECT_EQ(after.data[0]["shell"].get<std::string>(), std::string("bash"));

  // The grid comes from the log, with the escape sequences stripped.
  FsResult g = api.terminal_grid("t_1");
  ASSERT_TRUE(g.ok());
  ASSERT_TRUE(g.data["lines"].is_array());
  ASSERT_TRUE(g.data["lines"].size() >= 2);
  std::string first = g.data["lines"][0][0]["t"].get<std::string>();
  EXPECT_EQ(first, std::string("hello"));
  std::string second = g.data["lines"][1][0]["t"].get<std::string>();
  EXPECT_EQ(second, std::string("world"));
  EXPECT_EQ(g.data["status"].get<std::string>(), std::string("exited"));

  // ---- 轮转档与输入日志：历史"压缩"但不丢，搜索两路都查 ----
  // A rotated archive (older) + the separate typed-input log, exactly what the
  // live app leaves after a long session.
  {
    std::ofstream ofs((tdir / "t_1.1700000000000.log").string(), std::ios::binary);
    ofs << "ancient line with needle\r\n";
  }
  {
    std::ofstream ofs((tdir / "t_1.input.log").string(), std::ios::binary);
    ofs << "echo typed-needle\r\n";
  }
  // The replay reads the WHOLE chain: the archive comes before the current log.
  FsResult g2 = api.terminal_grid("t_1");
  ASSERT_TRUE(g2.ok());
  bool saw_ancient = false, saw_hello = false;
  for (size_t i = 0; i < g2.data["lines"].size(); ++i) {
    if (!g2.data["lines"][i].is_array() || g2.data["lines"][i].empty()) continue;
    std::string t = g2.data["lines"][i][0]["t"].get<std::string>();
    if (t.find("ancient line") != std::string::npos) saw_ancient = true;
    if (t.find("hello") != std::string::npos) saw_hello = true;
  }
  EXPECT_TRUE(saw_ancient);
  EXPECT_TRUE(saw_hello);

  // Search the archive...
  FsResult tsr = api.terminal_search("t_1", "needle", 50);
  ASSERT_TRUE(tsr.ok());
  ASSERT_TRUE(tsr.data["matches"].is_array());
  EXPECT_TRUE(tsr.data["matches"].size() >= 2);
  bool saw_archive = false, saw_input = false;
  for (size_t i = 0; i < tsr.data["matches"].size(); ++i) {
    std::string f = tsr.data["matches"][i]["file"].get<std::string>();
    if (f.find(".1700000000000.log") != std::string::npos) saw_archive = true;
    if (f == "t_1.input.log") saw_input = true;
  }
  EXPECT_TRUE(saw_archive);   // the rotated history is still searched
  EXPECT_TRUE(saw_input);     // and what the USER typed is its own record
}
