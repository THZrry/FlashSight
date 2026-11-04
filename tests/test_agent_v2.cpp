// test_agent_v2.cpp - water-level compaction, edit protocol, usage accounting,
// step subagents
//
// These pin the behaviours the previous round did not have:
//   * compaction is decided by the CONTEXT WATER LEVEL (a fact) not by turn count,
//   * edits carry a hash + the expected text (stale writes are impossible),
//   * the usage view can report cache hit rate and per-turn cost,
//   * a checklist step can be delegated with its transcript kept out of the
//     main conversation.
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "agent/agent_context.hpp"
#include "agent/agent_host.hpp"
#include "agent/agent_loop.hpp"
#include "agent/history_builder.hpp"
#include "common/fs_hash.hpp"
#include "common/fs_json.hpp"
#include "core/usage_store.hpp"
#include "tools/file_tools.hpp"
#include "tools/tool_registry.hpp"

using namespace fs;

namespace {

Json user_turn(const std::string& text) {
  return Json{{"role", "user"}, {"promptRaw", text}};
}
Json asst_turn(const std::string& text) {
  return Json{{"role", "assistant"}, {"streamSummary", text}};
}
Json make_full(int pairs) {
  Json turns = Json::array();
  for (int i = 0; i < pairs; ++i) {
    turns.push_back(user_turn("question " + std::to_string(i) + " with some words"));
    turns.push_back(asst_turn("answer " + std::to_string(i)));
  }
  return Json{{"turns", turns}};
}

}  // namespace

// ===========================================================================
// water-level driven compaction
// ===========================================================================

// ---------- 水位到阈值 -> 压缩；未到 -> 不压缩 ----------
FS_TEST(compress_decision_uses_water_level) {
  HistoryBuilder::Options opts;
  opts.context_limit = 100000;
  opts.compress_at_pct = 80;

  // Below the threshold: no compaction.
  opts.last_prompt_tokens = 50000;  // 50%
  auto d1 = HistoryBuilder::should_compress(opts, 1000);
  EXPECT_FALSE(d1.compress);
  EXPECT_EQ(d1.pct, 50);

  // At the threshold: compact, and say why.
  opts.last_prompt_tokens = 80000;  // 80%
  auto d2 = HistoryBuilder::should_compress(opts, 1000);
  EXPECT_TRUE(d2.compress);
  EXPECT_EQ(d2.reason, std::string("water-level"));

  // Window unknown: nothing to measure against, so nothing compacts. The token
  // budget is deliberately NOT a trigger any more (see should_compress): it was a
  // second, invisible threshold that fired on long sessions regardless of how full
  // the window was, and providers handle a long context themselves.
  opts.context_limit = 0;
  opts.last_prompt_tokens = 0;
  auto d3 = HistoryBuilder::should_compress(opts, 500);
  EXPECT_FALSE(d3.compress);
  EXPECT_TRUE(d3.reason.empty());

  // Forced (the user pressed "compact").
  opts.force_compress = true;
  auto d4 = HistoryBuilder::should_compress(opts, 1);
  EXPECT_TRUE(d4.compress);
  EXPECT_EQ(d4.reason, std::string("forced"));
}

// ---------- 水位触发时压缩确实发生，且最近 keep_turns 轮全量保留 ----------
FS_TEST(compress_water_level_keeps_recent_turns) {
  HistoryBuilder::Options opts;
  opts.system_prefix = "sys";
  opts.context_limit = 10000;
  opts.compress_at_pct = 80;
  opts.last_prompt_tokens = 9000;  // 90% -> water-level trigger
  // keep_turns counts STORED turns (user and assistant each count): 4 keeps the
  // last two exchanges.
  opts.keep_turns = 4;
  HistoryBuilder hb(opts);
  Json full = make_full(20);
  HistoryResult hr = hb.build_neutral(full);
  EXPECT_TRUE(hr.compressed);
  // The two newest exchanges must be present verbatim.
  std::string all;
  for (auto& m : hr.messages) all += jsonutil::get<std::string>(m, "content", "");
  EXPECT_TRUE(all.find("answer 19") != std::string::npos);
  EXPECT_TRUE(all.find("answer 18") != std::string::npos);
  // And the oldest must be gone (folded into the summary).
  EXPECT_TRUE(all.find("question 0 with") == std::string::npos);
  // The fold is the second message.
  EXPECT_TRUE(jsonutil::get<std::string>(hr.messages[1], "content", "")
                  .find("compacted") != std::string::npos);
}

// ---------- 压缩会真的让请求变小（不是只打个标记）----------
FS_TEST(compress_actually_shrinks_messages) {
  HistoryBuilder::Options opts;
  opts.system_prefix = "sys";
  opts.context_limit = 10000;
  opts.compress_at_pct = 80;
  opts.keep_turns = 2;
  Json full = make_full(30);
  // Uncompressed size.
  opts.last_prompt_tokens = 0;
  HistoryResult big = HistoryBuilder(opts).build_neutral(full);
  // Compressed size.
  opts.last_prompt_tokens = 9500;
  HistoryResult small = HistoryBuilder(opts).build_neutral(full);
  EXPECT_TRUE(small.compressed);
  EXPECT_TRUE(small.est_tokens < big.est_tokens);
}

// ---------- 强制压缩：轮数很少也能压 ----------
FS_TEST(compress_forced_with_few_turns) {
  HistoryBuilder::Options opts;
  opts.system_prefix = "sys";
  opts.keep_turns = 1;
  opts.force_compress = true;
  Json full = make_full(4);
  HistoryResult hr = HistoryBuilder(opts).build_neutral(full);
  EXPECT_TRUE(hr.compressed);
  EXPECT_TRUE(hr.dropped_turns > 0);
}

// ---------- loop：水位到达阈值时发出 context.compressed ----------
FS_TEST(loop_emits_compressed_event_on_water_level) {
  AgentLoop::Options opts;
  opts.sid = "s_wl";
  opts.permission = "full";
  opts.history.system_prefix = "sys";
  opts.history.keep_turns = 1;
  opts.context.auto_mode = false;
  std::vector<std::string> events;
  std::vector<Json> payloads;
  opts.event = [&](const std::string& e, const Json& p) {
    events.push_back(e);
    if (e == "context.compressed") payloads.push_back(p);
  };
  AgentLoop loop(std::move(opts));
  loop.context_limit = 10000;
  loop.set_last_prompt_tokens(9500);  // 95% water level
  Json full = make_full(10);
  loop.load_history = [&full](Json* out) -> FsResult {
    *out = full;
    return FsResult::success();
  };
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    out->text = "ok";
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("next").ok());
  ASSERT_TRUE(!payloads.empty());
  EXPECT_EQ(jsonutil::get<std::string>(payloads[0], "reason", ""),
            std::string("water-level"));
  EXPECT_TRUE(jsonutil::get_int(payloads[0], "waterPct", 0) >= 90);
}

// ---------- loop：request_compress 下一轮强制压缩 ----------
FS_TEST(loop_request_compress_forces_next_turn) {
  AgentLoop::Options opts;
  opts.sid = "s_force";
  opts.permission = "full";
  opts.history.system_prefix = "sys";
  opts.history.keep_turns = 1;
  std::vector<Json> payloads;
  opts.event = [&](const std::string& e, const Json& p) {
    if (e == "context.compressed") payloads.push_back(p);
  };
  AgentLoop loop(std::move(opts));
  Json full = make_full(10);
  loop.load_history = [&full](Json* out) -> FsResult {
    *out = full;
    return FsResult::success();
  };
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    out->text = "ok";
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  loop.request_compress();
  ASSERT_TRUE(loop.compress_requested());
  ASSERT_TRUE(loop.send("go").ok());
  EXPECT_FALSE(loop.compress_requested());  // consumed once
  ASSERT_TRUE(!payloads.empty());
  EXPECT_EQ(jsonutil::get<std::string>(payloads[0], "reason", ""),
            std::string("forced"));
}

// ===========================================================================
// edit protocol
// ===========================================================================

namespace {

struct FileFixture {
  fstest::TempDir td{"edit"};
  ghc::filesystem::path proj;
  ToolRegistry reg;
  ToolContext ctx;

  explicit FileFixture(const std::string& content, const std::string& name = "a.txt") {
    proj = ghc::filesystem::path(td.path()) / "proj";
    ghc::filesystem::create_directories(proj);
    std::ofstream ofs((proj / name).string(), std::ios::binary);
    ofs << content;
    register_file_tools(&reg);
    ctx.cwd = proj.string();
  }
  FsResult read(const std::string& path = "a.txt") {
    return reg.invoke(ctx, "read_file", Json{{"path", path}});
  }
  FsResult edit(Json args) { return reg.invoke(ctx, "edit_file", args); }
  std::string text(const std::string& path = "a.txt") {
    std::ifstream ifs((proj / path).string(), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(ifs)),
                  std::istreambuf_iterator<char>());
    return s;
  }
};

}  // namespace

// ---------- 编辑保留文件末尾换行（否则整文件 diff）----------
FS_TEST(edit_preserves_trailing_newline) {
  FileFixture f("alpha\nbeta\ngamma\n");
  FsResult r = f.read();
  ASSERT_TRUE(r.ok());
  FsResult e = f.edit(Json{{"path", "a.txt"},
                           {"hash", r.data["hash"]},
                           {"edits", Json::array({Json{{"start", 2},
                                                       {"expect", "beta"},
                                                       {"newText", "BETA"}}})}});
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(f.text(), std::string("alpha\nBETA\ngamma\n"));
}

// ---------- 没有末尾换行的文件不会被补上 ----------
FS_TEST(edit_does_not_add_trailing_newline) {
  FileFixture f("alpha\nbeta");
  FsResult r = f.read();
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(f.edit(Json{{"path", "a.txt"},
                          {"hash", r.data["hash"]},
                          {"edits", Json::array({Json{{"start", 1},
                                                      {"expect", "alpha"},
                                                      {"newText", "ALPHA"}}})}})
                  .ok());
  EXPECT_EQ(f.text(), std::string("ALPHA\nbeta"));
}

// ---------- CRLF 文件编辑后统一成 LF（默认 lineEnding=lf） ----------
// The user's call: "crlf最好全用\n避免兼容问题" - the model writes \n, and a CRLF
// file produced a whole class of edit/expect mismatches. "preserve" is still
// available by asking for it explicitly.
FS_TEST(edit_normalises_crlf_to_lf) {
  FileFixture f("alpha\r\nbeta\r\n");
  FsResult r = f.read();
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data.value("eol", ""), std::string("crlf"));
  ASSERT_TRUE(f.edit(Json{{"path", "a.txt"},
                          {"hash", r.data["hash"]},
                          {"edits", Json::array({Json{{"start", 2},
                                                      {"expect", "beta"},
                                                      {"newText", "BETA"}}})}})
                  .ok());
  EXPECT_EQ(f.text(), std::string("alpha\nBETA\n"));
}

// ---------- 多行插入：换行符跟随文件 ----------
FS_TEST(edit_inserts_multiple_lines) {
  FileFixture f("one\ntwo\n");
  FsResult r = f.read();
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(f.edit(Json{{"path", "a.txt"},
                          {"hash", r.data["hash"]},
                          {"edits", Json::array({Json{{"start", 2},
                                                      {"expect", "two"},
                                                      {"newText", "two\nthree\nfour"}}})}})
                  .ok());
  EXPECT_EQ(f.text(), std::string("one\ntwo\nthree\nfour\n"));
}

// ---------- 多个编辑按原始行号解释（不会互相错位）----------
FS_TEST(edit_multiple_edits_use_original_line_numbers) {
  FileFixture f("l1\nl2\nl3\nl4\nl5\n");
  FsResult r = f.read();
  ASSERT_TRUE(r.ok());
  FsResult e = f.edit(
      Json{{"path", "a.txt"},
           {"hash", r.data["hash"]},
           {"edits", Json::array({
               Json{{"start", 1}, {"expect", "l1"}, {"newText", "L1"}},
               Json{{"start", 3}, {"expect", "l3"}, {"newText", "L3\nL3b"}},
               Json{{"start", 5}, {"expect", "l5"}, {"newText", "L5"}},
           })}});
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(f.text(), std::string("L1\nl2\nL3\nL3b\nl4\nL5\n"));
}

// ---------- hash 过期 -> 拒绝且不写盘 ----------
FS_TEST(edit_stale_hash_refuses_and_keeps_file) {
  FileFixture f("alpha\nbeta\n");
  FsResult r = f.read();
  ASSERT_TRUE(r.ok());
  std::string original = f.text();
  // Someone else edits the file.
  {
    std::ofstream ofs((f.proj / "a.txt").string(), std::ios::binary);
    ofs << "changed by someone else\n";
  }
  FsResult e = f.edit(Json{{"path", "a.txt"},
                           {"hash", r.data["hash"]},
                           {"edits", Json::array({Json{{"start", 1},
                                                       {"newText", "hijacked"}}})}});
  EXPECT_FALSE(e.ok());
  EXPECT_TRUE(e.data.value("stale", false));
  EXPECT_EQ(f.text(), std::string("changed by someone else\n"));
}

// ---------- 只读代理不能编辑 ----------
FS_TEST(edit_denied_outside_project_without_approval) {
  FileFixture f("alpha\n");
  f.ctx.permission = "permissive";
  f.ctx.approved = false;
  FsResult e = f.edit(Json{{"path", "../outside.txt"},
                           {"edits", Json::array({Json{{"start", 1},
                                                       {"newText", "x"}}})}});
  EXPECT_FALSE(e.ok());
}

// ===========================================================================
// usage accounting
// ===========================================================================

// ---------- 缓存命中率：只统计报告过 cache 的行 ----------
FS_TEST(usage_cache_hit_ratio) {
  fstest::TempDir td("usage");
  ghc::filesystem::path file = ghc::filesystem::path(td.path()) / "calls.jsonl";
  UsageStore store(file);
  // Two rows with cache, one without (an old provider).
  ASSERT_TRUE(store.record(Json{{"ts", 1000}, {"sid", "s1"}, {"provider", "p"},
                                {"model", "m"}, {"in", 1000}, {"out", 100},
                                {"cache", 800}, {"cost", 0.01},
                                {"cost_kind", "money"}}).ok());
  ASSERT_TRUE(store.record(Json{{"ts", 2000}, {"sid", "s1"}, {"provider", "p"},
                                {"model", "m"}, {"in", 1000}, {"out", 100},
                                {"cache", 900}, {"cost", 0.01},
                                {"cost_kind", "money"}}).ok());
  ASSERT_TRUE(store.record(Json{{"ts", 3000}, {"sid", "s1"}, {"provider", "q"},
                                {"model", "m2"}, {"in", 500}, {"out", 50}}).ok());
  UsageFilter f;
  f.sid = "s1";
  UsageSummary sum = store.query(f);
  EXPECT_TRUE(sum.cache_reported);
  EXPECT_EQ(sum.total_cache, static_cast<int64_t>(1700));
  // 1700 cached / 2000 prompt tokens that reported cache = 0.85
  EXPECT_TRUE(sum.cache_hit_ratio > 0.84 && sum.cache_hit_ratio < 0.86);
  // The row that never reported cache still counts for tokens/calls.
  EXPECT_EQ(sum.total_in, static_cast<int64_t>(2500));
  EXPECT_EQ(sum.calls, static_cast<uint64_t>(3));
}

// ---------- 从未报告 cache -> -1（不是 0%）----------
FS_TEST(usage_cache_unknown_is_minus_one) {
  fstest::TempDir td("usage2");
  ghc::filesystem::path file = ghc::filesystem::path(td.path()) / "calls.jsonl";
  UsageStore store(file);
  ASSERT_TRUE(store.record(Json{{"ts", 1000}, {"sid", "s2"}, {"provider", "p"},
                                {"model", "m"}, {"in", 100}, {"out", 10}}).ok());
  UsageFilter f;
  f.sid = "s2";
  UsageSummary sum = store.query(f);
  EXPECT_FALSE(sum.cache_reported);
  EXPECT_TRUE(sum.cache_hit_ratio < 0);
}

// ---------- 会话成本按 provider 区分 money / quota ----------
FS_TEST(usage_cost_kind_per_provider) {
  fstest::TempDir td("usage3");
  ghc::filesystem::path file = ghc::filesystem::path(td.path()) / "calls.jsonl";
  UsageStore store(file);
  ASSERT_TRUE(store.record(Json{{"ts", 1000}, {"sid", "s3"}, {"provider", "paid"},
                                {"model", "a"}, {"in", 10}, {"out", 1},
                                {"cost", 0.5}, {"cost_kind", "money"}}).ok());
  ASSERT_TRUE(store.record(Json{{"ts", 2000}, {"sid", "s3"}, {"provider", "plan"},
                                {"model", "b"}, {"in", 10}, {"out", 1},
                                {"cost", 0}, {"cost_kind", "quota"}}).ok());
  UsageFilter f;
  f.sid = "s3";
  UsageSummary sum = store.query(f);
  ASSERT_TRUE(sum.cost_kind_by_provider.count("paid") > 0);
  EXPECT_EQ(sum.cost_kind_by_provider["paid"], std::string("money"));
  EXPECT_EQ(sum.cost_kind_by_provider["plan"], std::string("quota"));
  // The aggregate kind is "money" (it wins) but the per-provider split keeps
  // the distinction: adding ¥0.5 to a quota count would be meaningless.
  EXPECT_EQ(sum.cost_kind, std::string("money"));
}

// ===========================================================================
// hash utility
// ===========================================================================

FS_TEST(hash_xxh64_basics) {
  // Stable, distinguishes inputs, and short_hex truncates.
  std::string a = hash::xxh64_hex("hello");
  std::string b = hash::xxh64_hex("hello");
  std::string c = hash::xxh64_hex("hellp");
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_EQ(a.size(), static_cast<size_t>(16));
  EXPECT_EQ(hash::short_hex(a).size(), static_cast<size_t>(12));
  // Known vector: xxh64("", 0) = 0xef46db3751d8e999
  EXPECT_EQ(hash::xxh64_hex(""), std::string("ef46db3751d8e999"));
}

// ===========================================================================
// step subagents
// ===========================================================================

// ---------- 子代理上下文：只做一项，且带清单项 ----------
FS_TEST(subagent_context_is_step_scoped) {
  ContextInput in;
  in.lang = ContextLang::kEn;
  in.role = "subagent";
  in.subagent_kind = "complete-step";
  in.subagent_step = "add the retry logic";
  in.subagent_task = "edit src/net.cpp";
  std::string sys = AgentContext::build_system(in);
  EXPECT_TRUE(sys.find("add the retry logic") != std::string::npos);
  EXPECT_TRUE(sys.find("Do only this item") != std::string::npos);
  // A step subagent must not be told to plan the whole task again.
  EXPECT_TRUE(sys.find("Do not widen the scope") != std::string::npos);
}

// ---------- 主代理建议里包含"每步派一个 complete-step" ----------
FS_TEST(main_context_recommends_step_delegation) {
  ContextInput in;
  in.lang = ContextLang::kEn;
  in.allow_subagents = true;
  in.max_subagent_parallel = 4;
  in.max_subagent_depth = 3;
  std::string sys = AgentContext::build_system(in);
  EXPECT_TRUE(sys.find("Delegate each step") != std::string::npos);
  EXPECT_TRUE(sys.find("complete-step") != std::string::npos);
  EXPECT_TRUE(sys.find("todoId") != std::string::npos);
  // Chinese variant carries the same instruction.
  ContextInput zh = in;
  zh.lang = ContextLang::kZh;
  std::string zsys = AgentContext::build_system(zh);
  EXPECT_TRUE(zsys.find("每一步单独派一个 complete-step") != std::string::npos);
}
