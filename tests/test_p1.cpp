// test_p1.cpp - P1 AppCore unit tests (M1 验收 + build.md §4 重点项)
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "core/fs_api.hpp"
#include "core/settings_store.hpp"
#include "core/lock_arbiter.hpp"
#include "core/terminal_registry.hpp"
#include "core/deny_list.hpp"
#include "core/usage_store.hpp"
#include "common/fs_clock.hpp"
#include "common/fs_path.hpp"

using namespace fs;

namespace {

// 假时钟：可手动推进（单测不碰真时钟）
class FakeClock : public Clock {
 public:
  explicit FakeClock(ClockMs start = 1000000) : now_(start) {}
  ClockMs now_ms() const override { return now_; }
  void advance(ClockMs d) { now_ += d; }
  void set(ClockMs t) { now_ = t; }

 private:
  ClockMs now_;
};

// 固定种子随机：sid 可复现
class FixedRandom : public Random {
 public:
  explicit FixedRandom(uint64_t seed) : state_(seed ? seed : 1) {}
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
        out.push_back(hexd[(v >> (i * 4)) & 0xf]);
      }
    }
    return out;
  }

 private:
  uint64_t state_;
};

}  // namespace

// ---------- M1 验收：建空会话→写几轮→读回→回滚→分叉→遗忘 ----------
FS_TEST(m1_full_flow) {
  fstest::TempDir td("m1");
  FakeClock clock(1000);
  FixedRandom rng(42);
  FsApi api(ghc::filesystem::path(td.path()), &clock, &rng);

  // 建项目
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  EXPECT_FALSE(pid.empty());

  // 建空会话
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();
  EXPECT_TRUE(sid.compare(0, 2, "s_") == 0);

  // 读回 full.json：meta + sessionSettings 固化 + 空 turns
  FsResult fr = api.session_get(sid);
  ASSERT_TRUE(fr.ok());
  EXPECT_EQ(fr.data["meta"]["sid"].get<std::string>(), sid);
  EXPECT_EQ(fr.data["meta"]["pid"].get<std::string>(), pid);
  EXPECT_TRUE(fr.data["turns"].is_array());
  EXPECT_EQ((int)fr.data["turns"].size(), 0);
  EXPECT_TRUE(fr.data["sessionSettings"].is_object());

  // 写 3 轮（两轮带 patch，第三轮不带）
  ProjectStore ps = api.sessions().project_store_of(sid);
  SessionFileStore store(ps);
  for (int i = 0; i < 3; ++i) {
    Json turn = Json::object();
    turn["turnId"] = "t" + std::to_string(i + 1);
    turn["role"] = "user";
    turn["time"] = clock.now_ms();
    clock.advance(1000);
    EXPECT_TRUE(api.session_append_turn(sid, turn).ok());
  }
  // 模拟 t2 修改了文件 foo.txt（写 patch 记录，供回滚）
  Json patch;
  {
    Json c = Json::object();
    c["op"] = "write";
    c["path"] = "foo.txt";
    c["before"] = "orig\n";
    c["after"] = "t2edit\n";
    patch = Json{{"turnId", "t2"}, {"changes", Json::array({c})}};
  }
  EXPECT_TRUE(store.write_patch(sid, "t2", patch).ok());
  // 实际文件内容 = after（模拟工具已写盘；二进制模式，避免 Windows 文本模式 CRLF 翻译）
  {
    std::ofstream ofs((proj_dir / "foo.txt").string(), std::ios::binary);
    ofs << "t2edit\n";
  }

  // 读回：3 轮
  fr = api.session_get(sid);
  ASSERT_TRUE(fr.ok());
  EXPECT_EQ((int)fr.data["turns"].size(), 3);

  // 回滚到 t2（删 t2、t3，还原 foo.txt）
  FsResult rb = api.session_rollback(sid, "t2");
  ASSERT_TRUE(rb.ok());
  EXPECT_TRUE(rb.data["rolledBack"].get<bool>());
  fr = api.session_get(sid);
  ASSERT_TRUE(fr.ok());
  EXPECT_EQ((int)fr.data["turns"].size(), 1);
  // foo.txt 还原为 before
  std::string content;
  {
    std::ifstream ifs((proj_dir / "foo.txt").string(), std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    content = ss.str();
  }
  EXPECT_EQ(content, std::string("orig\n"));

  // 分叉：从 t1 复制前缀 → 新 sid，原会话不动
  FsResult fk = api.session_fork(sid, "t1");
  ASSERT_TRUE(fk.ok());
  std::string fork_sid = fk.data["sid"].get<std::string>();
  EXPECT_NE(fork_sid, sid);
  FsResult ff = api.session_get(fork_sid);
  ASSERT_TRUE(ff.ok());
  EXPECT_EQ(ff.data["meta"]["sid"].get<std::string>(), fork_sid);
  EXPECT_EQ((int)ff.data["turns"].size(), 1);

  // 遗忘：单条（标 forgotten 不物理删）
  EXPECT_TRUE(api.session_forget(fork_sid, "t1").ok());
  ff = api.session_get(fork_sid);
  ASSERT_TRUE(ff.ok());
  EXPECT_TRUE(ff.data["turns"][0]["forgotten"].get<bool>());
  // 全部遗忘
  EXPECT_TRUE(api.session_forget(fork_sid, "all").ok());
  ff = api.session_get(fork_sid);
  ASSERT_TRUE(ff.ok());
  EXPECT_TRUE(ff.data["turns"][0]["forgotten"].get<bool>());
  // 遗忘不存在的 turn → 报错
  EXPECT_FALSE(api.session_forget(fork_sid, "nope").ok());
}

// ---------- session 设置：固化 + 权限升级确认 ----------
FS_TEST(session_settings_permission_upgrade) {
  fstest::TempDir td("perm");
  FakeClock clock(1000);
  FixedRandom rng(7);
  FsApi api(ghc::filesystem::path(td.path()), &clock, &rng);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  std::string pid = api.project_create(proj_dir.string()).data["pid"].get<std::string>();
  std::string sid = api.session_new(pid, proj_dir.string()).data["sid"].get<std::string>();

  // 默认 permissive
  Json full = api.session_get(sid).data;
  EXPECT_EQ(full["sessionSettings"]["filePerm"].get<std::string>(), "permissive");

  // 升级到 full 需确认令牌
  FsResult deny = api.session_set_permission(sid, "full");
  EXPECT_TRUE(deny.denied());
  EXPECT_EQ(deny.denial.reason, std::string("confirm-required"));

  // 申请令牌 → 确认
  FsResult tok = api.session_request_permission_token(sid, "full");
  ASSERT_TRUE(tok.ok());
  std::string token = tok.data["token"].get<std::string>();
  EXPECT_FALSE(token.empty());
  EXPECT_TRUE(api.session_set_permission_confirm(sid, "full", token).ok());
  full = api.session_get(sid).data;
  EXPECT_EQ(full["sessionSettings"]["filePerm"].get<std::string>(), "full");

  // 错误令牌 → 拒绝
  FsResult bad = api.session_set_permission_confirm(sid, "approve", "badtoken");
  EXPECT_TRUE(bad.code == ErrCode::kConfirmRequired);

  // 降级不需要令牌
  EXPECT_TRUE(api.session_set_permission(sid, "approve").ok());
  full = api.session_get(sid).data;
  EXPECT_EQ(full["sessionSettings"]["filePerm"].get<std::string>(), "approve");

  // 非法权限名
  EXPECT_FALSE(api.session_set_permission(sid, "bogus").ok());

  // systemOps 落库：至少 2 条（setPermission x2）
  full = api.session_get(sid).data;
  EXPECT_TRUE(full["systemOps"].is_array());
  EXPECT_TRUE((int)full["systemOps"].size() >= 2);
}

// ---------- refreshConfig：configVersion+1、清连续失败、记 systemOps ----------
FS_TEST(session_refresh_config) {
  fstest::TempDir td("refresh");
  FakeClock clock(1000);
  FixedRandom rng(9);
  FsApi api(ghc::filesystem::path(td.path()), &clock, &rng);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  std::string pid = api.project_create(proj_dir.string()).data["pid"].get<std::string>();
  std::string sid = api.session_new(pid, proj_dir.string()).data["sid"].get<std::string>();

  Json full = api.session_get(sid).data;
  EXPECT_EQ((int)jsonutil::get_int(full, "configVersion", 0), 0);
  EXPECT_TRUE(api.session_refresh_config(sid).ok());
  full = api.session_get(sid).data;
  EXPECT_EQ((int)jsonutil::get_int(full, "configVersion", 0), 1);
  EXPECT_EQ((int)jsonutil::get_int(full, "continuousFailures", -1), 0);
  bool found = false;
  for (auto& op : full["systemOps"]) {
    if (jsonutil::get<std::string>(op, "op", "") == "refreshConfig") found = true;
  }
  EXPECT_TRUE(found);
}

// ---------- 回滚冲突：文件被外部改过 → 如实报错，不静默 ----------
FS_TEST(rollback_conflict_honest) {
  fstest::TempDir td("rbc");
  FakeClock clock(1000);
  FixedRandom rng(11);
  FsApi api(ghc::filesystem::path(td.path()), &clock, &rng);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  std::string pid = api.project_create(proj_dir.string()).data["pid"].get<std::string>();
  std::string sid = api.session_new(pid, proj_dir.string()).data["sid"].get<std::string>();

  Json turn = Json::object();
  turn["turnId"] = "t1";
  turn["role"] = "user";
  turn["time"] = clock.now_ms();
  EXPECT_TRUE(api.session_append_turn(sid, turn).ok());

  ProjectStore ps = api.sessions().project_store_of(sid);
  SessionFileStore store(ps);
  Json c = Json::object();
  c["op"] = "write";
  c["path"] = "bar.txt";
  c["before"] = "before\n";
  c["after"] = "after\n";
  store.write_patch(sid, "t1", Json{{"turnId", "t1"}, {"changes", Json::array({c})}});
  // 盘上内容被外部改成了 third-party 版本（≠ after）
  {
    std::ofstream ofs((proj_dir / "bar.txt").string());
    ofs << "externally-changed\n";
  }
  std::vector<std::string> errors;
  FsResult rb = api.sessions().rollback(sid, "t1", &errors);
  ASSERT_TRUE(rb.ok());  // 回滚动作完成，但有冲突记录
  EXPECT_TRUE(!errors.empty());  // 如实报错
  // 文件未被回滚（内容保持 externally-changed）
  std::string content;
  {
    std::ifstream ifs((proj_dir / "bar.txt").string());
    std::stringstream ss;
    ss << ifs.rdbuf();
    content = ss.str();
  }
  EXPECT_EQ(content, std::string("externally-changed\n"));
}

// ---------- 不存在的会话/turn：错误码 ----------
FS_TEST(errors_not_found) {
  fstest::TempDir td("err");
  FakeClock clock(1000);
  FixedRandom rng(13);
  FsApi api(ghc::filesystem::path(td.path()), &clock, &rng);
  EXPECT_TRUE(api.session_get("s_deadbeef").code == ErrCode::kNotFound);
  EXPECT_TRUE(api.project_get("p_nope").code == ErrCode::kNotFound);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  std::string pid = api.project_create(proj_dir.string()).data["pid"].get<std::string>();
  std::string sid = api.session_new(pid, proj_dir.string()).data["sid"].get<std::string>();
  EXPECT_TRUE(api.session_rollback(sid, "nonexistent").code == ErrCode::kNotFound);
  EXPECT_TRUE(api.session_fork(sid, "nonexistent").code == ErrCode::kNotFound);
  // 非法 sid 格式
  EXPECT_TRUE(api.session_get("bad-sid").code == ErrCode::kInvalidArg);
}

// ---------- 文件预览：文本 / 图片 / 二进制三分 ----------
// A preview tab used to decode EVERY file as text: a PNG opened as mojibake and
// a .exe as pages of garbage. The reader now says what the file IS.
FS_TEST(project_read_file_kinds) {
  fstest::TempDir td("preview");
  FakeClock clock(1000);
  FixedRandom rng(21);
  FsApi api(ghc::filesystem::path(td.path()), &clock, &rng);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  std::string pid = api.project_create(proj_dir.string()).data["pid"].get<std::string>();
  {
    std::ofstream f((proj_dir / "a.txt").string(), std::ios::binary);
    f << "hello\nworld\n";
  }
  FsResult r = api.project_read_file(pid, "a.txt");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data["kind"].get<std::string>(), std::string("text"));
  EXPECT_TRUE(r.data["content"].get<std::string>().find("hello") != std::string::npos);
  // A binary: NUL bytes in the head. No text content may be offered for it.
  {
    std::ofstream f((proj_dir / "bin.dat").string(), std::ios::binary);
    std::string bytes("MZ\x90\x00\x03\x00\x00\x00payload", 18);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  r = api.project_read_file(pid, "bin.dat");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data["kind"].get<std::string>(), std::string("binary"));
  EXPECT_TRUE(!r.data.contains("content"));
  // An image is recognised by its CONTENT (the sniffer only reads the header),
  // and comes back as a data URI the browser can paint.
  {
    std::ofstream f((proj_dir / "p.dat").string(), std::ios::binary);
    const unsigned char hdr[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    f.write(reinterpret_cast<const char*>(hdr), 8);
    f.write("fake-pixels", 11);
  }
  r = api.project_read_file(pid, "p.dat");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data["kind"].get<std::string>(), std::string("image"));
  std::string uri = r.data["dataUri"].get<std::string>();
  EXPECT_TRUE(uri.compare(0, 22, "data:image/png;base64,") == 0);
  EXPECT_EQ(r.data["mime"].get<std::string>(), std::string("image/png"));
  // UTF-16 text has NULs too, but on ONE parity - it is text, not binary.
  {
    std::ofstream f((proj_dir / "u16.txt").string(), std::ios::binary);
    const unsigned char bom[2] = {0xFF, 0xFE};
    f.write(reinterpret_cast<const char*>(bom), 2);
    const char txt[] = "h\0e\0l\0l\0o\0 \0w\0o\0r\0l\0d\0";
    f.write(txt, sizeof(txt) - 1);
  }
  r = api.project_read_file(pid, "u16.txt");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data["kind"].get<std::string>(), std::string("text"));
  EXPECT_TRUE(r.data["content"].get<std::string>().find("hello world") != std::string::npos);
}

// ---------- LockArbiter：同项目单写锁 + full 不设锁 + 超时兜底 ----------
FS_TEST(lock_arbiter_matrix) {
  FakeClock clock(1000);
  LockArbiter lock(&clock, 1000);  // 1s 超时
  std::string by;
  EXPECT_TRUE(lock.try_acquire("projA", "s_1", false, clock.now_ms(), &by));
  // 第二会话被拒并给出持有者
  EXPECT_FALSE(lock.try_acquire("projA", "s_2", false, clock.now_ms(), &by));
  EXPECT_EQ(by, std::string("s_1"));
  // 同会话重入
  EXPECT_TRUE(lock.try_acquire("projA", "s_1", false, clock.now_ms(), &by));
  // 另一个项目不受影响
  EXPECT_TRUE(lock.try_acquire("projB", "s_2", false, clock.now_ms(), &by));
  // full 权限不设锁
  EXPECT_TRUE(lock.try_acquire("projA", "s_3", true, clock.now_ms(), &by));
  // 释放后可获取
  lock.release("projA", "s_1");
  EXPECT_TRUE(lock.try_acquire("projA", "s_2", false, clock.now_ms(), &by));
  // 超时兜底：推进时间 > 超时
  clock.advance(5000);
  EXPECT_TRUE(lock.try_acquire("projA", "s_4", false, clock.now_ms(), &by));
  // release_all_for（会话关闭）
  lock.release_all_for("s_4");
  EXPECT_EQ(lock.holder("projA"), std::string(""));
}

// ---------- 黑白名单：黑 > 白 > 三档默认，拒绝可解释 ----------
FS_TEST(deny_list_match) {
  DenyList dl;
  ghc::filesystem::path root = pathutil::home_dir() / "proj";
  ghc::filesystem::path inside = root / "src" / "a.cpp";
  ghc::filesystem::path outside = pathutil::home_dir() / "other" / "b.cpp";

  dl.set_blacklist({(root / "secret").string()});
  PathMatch m = dl.match(inside);
  EXPECT_FALSE(m.denied);
  EXPECT_FALSE(m.whitelist_allow);
  m = dl.match(root / "secret" / "key.pem");
  EXPECT_TRUE(m.denied);
  EXPECT_FALSE(m.matched.empty());  // 可解释：命中的名单项

  // 白名单放行
  dl.set_whitelist({outside.string()});
  m = dl.match(outside);
  EXPECT_FALSE(m.denied);
  EXPECT_TRUE(m.whitelist_allow);

  // 黑 > 白：同路径同时命中 → 黑胜
  ghc::filesystem::path both = root / "secret2";
  dl.set_blacklist({both.string()});
  dl.set_whitelist({both.string()});
  m = dl.match(both / "x.txt");
  EXPECT_TRUE(m.denied);
}

// ---------- 终端注册表：统一编号 + 5min 晋升 + 编码切换 + 缺日志不报错 ----------
FS_TEST(terminal_registry_flow) {
  FakeClock clock(1000000);
  TerminalRegistry reg(&clock);
  // 用户终端
  TermEntry u = reg.create("s_1", TermKind::kUser, "cmd", "C:\\", "", false, 0);
  // 模型前台
  TermEntry fg = reg.create("s_1", TermKind::kModelFg, "cmd", "C:\\", "", false, 0);
  // 编号统一自增
  EXPECT_NE(u.tid, fg.tid);
  EXPECT_EQ(u.tid, std::string("1"));
  EXPECT_EQ(fg.tid, std::string("2"));
  // 5min 晋升：model-fg → model-bg
  EXPECT_TRUE(reg.promote_to_background(fg.tid, clock.now_ms()).ok());
  const TermEntry* fe = reg.find(fg.tid);
  ASSERT_TRUE(fe != nullptr);
  EXPECT_TRUE(fe->kind == TermKind::kModelBg);
  EXPECT_TRUE(fe->wake_on_exit);
  // 编码切换
  EXPECT_TRUE(reg.set_encoding(fg.tid, "utf-8").ok());
  EXPECT_EQ(reg.find(fg.tid)->encoding, std::string("utf-8"));
  // 退出标记
  EXPECT_TRUE(reg.mark_exited(fg.tid, 3, clock.now_ms()).ok());
  EXPECT_EQ(reg.find(fg.tid)->exit_code, 3);
  EXPECT_TRUE(reg.find(fg.tid)->status == TermStatus::kExited);
  // 列表
  auto list = reg.list_for_session("s_1");
  EXPECT_EQ((int)list.size(), 2);
  // 不存在的 tid
  EXPECT_TRUE(reg.mark_exited("999", 0, 0).code == ErrCode::kNotFound);
  EXPECT_TRUE(reg.set_encoding("999", "x").code == ErrCode::kNotFound);
}

// ---------- 用量：JSONL 坏行容忍 + 聚合 + CSV 导出 + 90 天清理 ----------
FS_TEST(usage_store_flow) {
  fstest::TempDir td("usage");
  ghc::filesystem::path calls = ghc::filesystem::path(td.path()) / "usage" / "calls.jsonl";
  UsageStore us(calls);
  // 直接写两行正常 + 一行坏行
  {
    ghc::filesystem::create_directories(calls.parent_path());
    std::ofstream ofs(calls.string(), std::ios::binary | std::ios::app);
    ofs << "{\"ts\":1000,\"provider\":\"p1\",\"model\":\"m1\",\"in\":10,\"out\":20,\"cost_kind\":\"money\"}\n";
    ofs << "this is a broken line not json\n";
    ofs << "{\"ts\":2000,\"provider\":\"p1\",\"model\":\"m2\",\"in\":5,\"out\":5,\"cost_kind\":\"unknown\"}\n";
  }
  UsageFilter f;
  UsageSummary sum = us.query(f);
  EXPECT_EQ((int)sum.calls, 2);
  EXPECT_EQ((int)sum.bad_lines, 1);  // 坏行容忍 + 计数
  EXPECT_EQ((int)sum.total_in, 15);
  EXPECT_EQ((int)sum.total_out, 25);
  EXPECT_EQ((int)sum.by_provider_model.size(), 2);
  // 聚合行要带调用次数：供应商没回报 usage（token 全 0）时，图表只能靠它，
  // 否则"有调用"会画成"什么都没发生"。
  for (auto& kv : sum.by_provider_model) EXPECT_EQ((int)kv.second.calls, 1);
  int day_calls = 0;
  for (auto& kv : sum.by_day) day_calls += (int)kv.second.calls;
  EXPECT_EQ(day_calls, 2);  // both rows fall in the same day bucket

  // 筛选
  f.model = "m1";
  sum = us.query(f);
  EXPECT_EQ((int)sum.calls, 1);
  f.model = "";

  // CSV 导出
  ghc::filesystem::path csv = ghc::filesystem::path(td.path()) / "out.csv";
  EXPECT_TRUE(us.export_csv(f, csv).ok());
  EXPECT_TRUE(ghc::filesystem::exists(csv));

  // 90 天清理：老记录删除，新保留
  UsageRow old_row;
  old_row.ts = 1000;  // 很老
  old_row.provider = "p1";
  old_row.model = "old";
  us.record(old_row);
  UsageRow new_row;
  new_row.ts = static_cast<int64_t>(std::time(nullptr)) * 1000;
  new_row.provider = "p1";
  new_row.model = "new";
  us.record(new_row);
  EXPECT_TRUE(us.purge_older_than_days(90).ok());
  f.model = "old";
  EXPECT_EQ((int)us.query(f).calls, 0);
  f.model = "new";
  EXPECT_EQ((int)us.query(f).calls, 1);
}

// ---------- Settings：两级 + 来源标注 + ignoreGlobal ----------
FS_TEST(settings_two_level) {
  fstest::TempDir td("set");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  ghc::filesystem::path proj = root / "proj";
  ghc::filesystem::create_directories(proj);
  SettingsStore s(root, pathutil::project_dir(proj), true);
  // 默认
  SettingValue sv = s.get(kKeyNewSessionFilePerm);
  EXPECT_EQ(sv.source, std::string("merged-default"));
  EXPECT_EQ(sv.value.get<std::string>(), std::string("permissive"));
  // 全局
  EXPECT_TRUE(s.set_global(kKeyNewSessionFilePerm, "approve").ok());
  sv = s.get(kKeyNewSessionFilePerm);
  EXPECT_EQ(sv.source, std::string("global"));
  EXPECT_TRUE(sv.global_set);
  // 项目覆盖全局
  EXPECT_TRUE(s.set_project(kKeyNewSessionFilePerm, "full").ok());
  sv = s.get(kKeyNewSessionFilePerm);
  EXPECT_EQ(sv.source, std::string("project"));
  // unset 项目回退全局
  EXPECT_TRUE(s.unset_project(kKeyNewSessionFilePerm).ok());
  sv = s.get(kKeyNewSessionFilePerm);
  EXPECT_EQ(sv.source, std::string("global"));
  // ignoreGlobal：忽略全局 → 默认值
  EXPECT_TRUE(s.set_ignore_global(true).ok());
  sv = s.get(kKeyNewSessionFilePerm);
  EXPECT_EQ(sv.source, std::string("merged-default"));
  EXPECT_TRUE(s.set_ignore_global(false).ok());
  sv = s.get(kKeyNewSessionFilePerm);
  EXPECT_EQ(sv.source, std::string("global"));
}

// ---------- 项目注册表：路径去重 + 列表 + 删除 ----------
FS_TEST(project_registry_flow) {
  fstest::TempDir td("proj");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  ghc::filesystem::create_directories(root / "a");
  ghc::filesystem::create_directories(root / "b");
  FixedRandom rng(5);
  FsApi api(root, nullptr, &rng);
  std::string p1 = api.project_open((root / "a").string()).data["pid"].get<std::string>();
  // 同路径重复 open → 同 pid
  std::string p1b = api.project_open((root / "a").string()).data["pid"].get<std::string>();
  EXPECT_EQ(p1, p1b);
  std::string p2 = api.project_create((root / "b").string()).data["pid"].get<std::string>();
  EXPECT_NE(p1, p2);
  // 列表 2 个
  Json list = api.project_list().data;
  EXPECT_EQ((int)list.size(), 2);
  // 删除
  EXPECT_TRUE(api.project_remove(p2).ok());
  list = api.project_list().data;
  EXPECT_EQ((int)list.size(), 1);
  // 不存在的项目上建会话 → 报错
  EXPECT_TRUE(api.session_new("p_gone", "").code == ErrCode::kNotFound);
}