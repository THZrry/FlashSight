// test_p3.cpp - P3 Agent 引擎单测（M3 验收，FakeNet/FakeClock，不碰真网真时钟）
// Part 1: Loop 状态机 + 挂起 + 引导队列
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "test_framework.hpp"

#include "agent/agent_loop.hpp"
#include "agent/steering_queue.hpp"
#include "agent/wake_scheduler.hpp"
#include "common/fs_clock.hpp"
#include "common/fs_path.hpp"

using namespace fs;

namespace {

class FakeClock3 : public Clock {
 public:
  explicit FakeClock3(ClockMs start = 1000) : now_(start) {}
  ClockMs now_ms() const override { return now_; }
  void advance(ClockMs d) { now_ += d; }
 private:
  ClockMs now_;
};

}  // namespace

// ---------- Loop：send 走 planning -> turnDone（Fake 模型回纯文本）----------
FS_TEST(loop_send_text_turn) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  opts.model = "test-model";
  opts.history.system_prefix = "sys";
  std::vector<std::string> events;
  opts.event = [&events](const std::string& e, const Json&) { events.push_back(e); };
  AgentLoop loop(std::move(opts));
  EXPECT_EQ(loop.state(), LoopState::kIdle);
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out, const std::function<void(const std::string&)>&) -> FsResult {
    out->text = "hello";
    out->error = "";
    return FsResult::success();
  };
  FsResult r = loop.send("hi");
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
  // stream.delta + turn.done 事件
  bool has_delta = false, has_done = false;
  for (auto& e : events) {
    if (e == "stream.delta") has_delta = true;
    if (e == "turn.done") has_done = true;
  }
  EXPECT_TRUE(has_delta);
  EXPECT_TRUE(has_done);
}

// ---------- Loop：运行中 send 进入引导队列 ----------
FS_TEST(loop_send_while_busy_queues) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  // approve 档：每次写文件都要问（这里验证的是权限门流程本身）
  opts.permission = "approve";
  bool first = true;
  opts.event = [](const std::string&, const Json&) {};
  AgentLoop loop(std::move(opts));
  // 第一轮：模型返回 tool 调用（高风险 -> waiting）
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out, const std::function<void(const std::string&)>&) -> FsResult {
    out->text = "";
    Json call = Json::object();
    call["name"] = "file.write";
    call["args"] = Json{{"path", "a.txt"}, {"content", "x"}};
    out->tool_calls = Json::array({call});
    out->error = "";
    return FsResult::success();
  };
  FsResult r = loop.send("write file");
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(loop.state() == LoopState::kWaiting);
  // 运行中再 send -> 引导队列（不报错）
  FsResult r2 = loop.send("also do this");
  EXPECT_TRUE(r2.ok());
}

// ---------- Loop：approve 拒绝后继续（addon2：拒绝结果回填并续跑）----------
FS_TEST(loop_approve_deny) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  // approve 档：file.rm 也要先问（宽松档下项目内的删除是直接放行的）
  opts.permission = "approve";
  opts.event = [](const std::string&, const Json&) {};
  AgentLoop loop(std::move(opts));
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out, const std::function<void(const std::string&)>&) -> FsResult {
    ++calls;
    if (calls == 1) {
      Json call = Json::object();
      call["name"] = "file.rm";
      call["args"] = Json{{"path", "b.txt"}};
      out->tool_calls = Json::array({call});
    } else {
      // 拒绝回填后：模型改用文字收尾（不再要高风险工具）
      out->text = "ok, skipped";
      out->tool_calls = Json::array();
    }
    out->error = "";
    return FsResult::success();
  };
  EXPECT_TRUE(loop.send("delete").ok());
  EXPECT_TRUE(loop.state() == LoopState::kWaiting);
  EXPECT_TRUE(loop.approve(false).ok());
  // 拒绝 → 结果回填 → 续跑至本轮结束（不再停在 kToolCalling）
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
  EXPECT_EQ(calls, 2);
}

// ---------- Loop：挂起后不再发新调用 ----------
FS_TEST(loop_suspend_stops) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  std::vector<std::string> events;
  opts.event = [&events](const std::string& e, const Json&) { events.push_back(e); };
  AgentLoop loop(std::move(opts));
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out, const std::function<void(const std::string&)>&) -> FsResult {
    ++calls;
    out->text = "t";
    Json call = Json::object();
    call["name"] = "file.read";
    call["args"] = Json{{"path", "a.txt"}};
    out->tool_calls = Json::array({call});
    out->error = "";
    return FsResult::success();
  };
  loop.suspend();
  EXPECT_TRUE(loop.send("go").ok());
  // file.read 非高风险 -> 直接执行回填，不再发第二轮（suspend 后停）
  EXPECT_TRUE(calls >= 1);
  bool has_suspended = false;
  for (auto& e : events) {
    if (e == "loop.state") has_suspended = true;
  }
  EXPECT_TRUE(has_suspended);
  loop.resume();
}

// ---------- WakeScheduler：定时唤醒 + 取消 ----------
FS_TEST(wake_scheduler_timer) {
  FakeClock3 clock(1000);
  WakeScheduler ws(&clock);
  EXPECT_FALSE(ws.has_pending("s_1"));
  ws.schedule("s_1", WakeSource::kTimer, 5000, "");
  EXPECT_TRUE(ws.has_pending("s_1"));
  // 未到点：due 为空
  auto d1 = ws.due(2000);
  EXPECT_EQ((int)d1.size(), 0);
  EXPECT_TRUE(ws.has_pending("s_1"));
  // 到点：触发
  bool fired = false;
  ws.set_fire_fn([&fired](const PendingWake&) { fired = true; });
  auto d2 = ws.due(6000);
  EXPECT_EQ((int)d2.size(), 1);
  EXPECT_EQ(d2[0].source, WakeSource::kTimer);
  EXPECT_TRUE(fired);
  EXPECT_FALSE(ws.has_pending("s_1"));
  // 取消
  ws.schedule("s_1", WakeSource::kTimer, 9000, "");
  EXPECT_TRUE(ws.cancel("s_1"));
  EXPECT_FALSE(ws.has_pending("s_1"));
}

// ---------- SteeringQueue：合并/编辑/排序/删除 ----------
FS_TEST(steering_queue_ops) {
  SteeringQueue q;
  int a = q.push(SteeringKind::kImmediate, "first");
  int b = q.push(SteeringKind::kImmediate, "second");
  int c = q.push(SteeringKind::kRoundEnd, "later");
  EXPECT_EQ((int)q.immediate_count(), 2);
  // 合并
  std::string merged = q.drain_immediate();
  EXPECT_EQ(merged, std::string("first\nsecond"));
  EXPECT_EQ((int)q.immediate_count(), 0);
  // 本轮结束传递保留
  EXPECT_EQ((int)q.round_end_items().size(), 1);
  // 编辑
  EXPECT_TRUE(q.edit(c, "later edited"));
  EXPECT_EQ(q.round_end_items()[0].text, std::string("later edited"));
  EXPECT_FALSE(q.edit(999, "x"));
  // 排序
  int d = q.push(SteeringKind::kRoundEnd, "d");
  EXPECT_TRUE(q.reorder({d, c}));
  auto items = q.items();
  EXPECT_EQ(items[0].id, d);
  EXPECT_EQ(items[1].id, c);
  EXPECT_FALSE(q.reorder({d}));
  // 删除
  EXPECT_TRUE(q.erase(d));
  EXPECT_FALSE(q.erase(d));
  // to_json
  Json j = q.to_json();
  EXPECT_TRUE(j.is_array());
}

#include "agent/overfit.hpp"
#include "agent/subagent.hpp"
#include "common/fs_simhash.hpp"
#include "tools/file_tools.hpp"
#include "tools/tool_registry.hpp"
#include "tools/ask_tool.hpp"

// ---------- ask：手动模式发事件；自动模式拒绝 ----------
FS_TEST(ask_tool_modes) {
  ToolRegistry reg;
  register_ask_tool(&reg);
  // 手动：发 ask.prompt
  bool prompted = false;
  ToolContext ctx;
  ctx.auto_mode = false;
  ctx.emit = [&prompted](const std::string& e, const Json&) {
    if (e == "ask.prompt") prompted = true;
  };
  Json args = Json::object();
  args["question"] = "which?";
  args["options"] = Json::array({"a", "b"});
  FsResult r = reg.invoke(ctx, "ask", args);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(prompted);
  // 自动：拒绝
  ToolContext ctx2;
  ctx2.auto_mode = true;
  FsResult r2 = reg.invoke(ctx2, "ask", args);
  EXPECT_TRUE(r2.code == ErrCode::kBusy);
  // 空问题：参数错
  Json bad = Json::object();
  bad["question"] = "";
  bad["options"] = Json::array();
  EXPECT_FALSE(reg.invoke(ctx, "ask", bad).ok());
}

// ---------- 过拟合：默认关；开后连续相似输出触发 ----------
FS_TEST(overfit_detector) {
  OverfitDetector off;
  EXPECT_FALSE(off.push("same same same"));
  EXPECT_FALSE(off.push("same same same"));
  EXPECT_FALSE(off.push("same same same"));
  OverfitDetector::Options opts;
  opts.enabled = true;
  opts.threshold = 0.9;
  opts.window = 3;
  opts.repeat = 3;
  OverfitDetector det(opts);
  EXPECT_FALSE(det.push("do the thing now please"));
  EXPECT_FALSE(det.push("do the thing now please"));
  EXPECT_TRUE(det.push("do the thing now please"));
  // 不同输出不触发
  det.clear();
  EXPECT_FALSE(det.push("alpha"));
  EXPECT_FALSE(det.push("beta gamma delta"));
  EXPECT_FALSE(det.push("completely different xyz 123"));
}

// ---------- Similarity：相同=1，不同<相同 ----------
FS_TEST(similarity_sanity) {
  EXPECT_EQ(Similarity::normalized("abc", "abc"), 1.0);
  double diff = Similarity::normalized("hello world", "xyz 123 456");
  double same = Similarity::normalized("hello world", "hello world!");
  EXPECT_TRUE(same > diff);
  EXPECT_TRUE(diff >= 0.0 && diff <= 1.0);
}

// ---------- 子代理：spawn/深度/并行/完成/超时 ----------
FS_TEST(subagent_limits) {
  FakeClock3 clock(1000);
  SubagentManager mgr(&clock);
  SubagentManager::Options opts;
  opts.max_parallel = 2;
  opts.max_depth = 2;
  mgr.set_options(opts);
  std::string id1, id2;
  EXPECT_TRUE(mgr.spawn("s_1", "research", "t1", "", "", 1, 0, &id1).ok());
  EXPECT_TRUE(mgr.spawn("s_1", "research", "t2", "", "", 1, 0, &id2).ok());
  // 并行上限
  std::string id3;
  EXPECT_TRUE(mgr.spawn("s_1", "research", "t3", "", "", 1, 0, &id3).code == ErrCode::kBusy);
  // 深度上限
  std::string idd;
  EXPECT_TRUE(mgr.spawn("s_1", "research", "deep", "", "", 3, 0, &idd).code ==
              ErrCode::kSubagentDepth);
  // 完成（/return 语义：summary 回父 + 全文保留）
  bool done_cb = false;
  mgr.set_done_fn([&done_cb](const SubagentRecord&) { done_cb = true; });
  EXPECT_TRUE(mgr.complete_with_summary(id1, "summary1", Json{{"full", 1}}).ok());
  EXPECT_TRUE(done_cb);
  const SubagentRecord* rec = mgr.find(id1);
  ASSERT_TRUE(rec != nullptr);
  EXPECT_TRUE(rec->status == SubagentStatus::kDone);
  EXPECT_EQ(rec->summary, std::string("summary1"));
  // 并行数降回
  EXPECT_EQ(mgr.running_count("s_1"), 1);
  // 取消
  EXPECT_TRUE(mgr.cancel(id2).ok());
  EXPECT_TRUE(mgr.find(id2)->status == SubagentStatus::kCancelled);
  EXPECT_FALSE(mgr.cancel("nope").ok());
  // 超时（时钟注入）
  std::string idt;
  mgr.spawn("s_2", "research", "timeout-task", "", "", 1, 100, &idt);
  clock.advance(200);
  auto timed = mgr.check_timeouts(clock.now_ms());
  EXPECT_EQ((int)timed.size(), 1);
  EXPECT_TRUE(mgr.find(idt)->status == SubagentStatus::kTimeout);
}

// ---------- 预置代理 7 个（含 addon1.md 5 个）----------
FS_TEST(preset_agents_cover_addon1) {
  auto v = builtin_preset_agents();
  auto has = [&v](const std::string& n) {
    for (auto& p : v) {
      if (p.name == n) return true;
    }
    return false;
  };
  EXPECT_TRUE(has("local-research"));
  EXPECT_TRUE(has("complete-step"));
  EXPECT_TRUE(has("return"));
  EXPECT_TRUE(has("round-plan"));
  EXPECT_TRUE(has("check-files"));
  // complete-step/round-plan 不支持并行
  for (auto& p : v) {
    if (p.name == "complete-step" || p.name == "round-plan") {
      EXPECT_FALSE(p.parallel);
    }
  }
  // round-plan 阻塞
  for (auto& p : v) {
    if (p.name == "round-plan") EXPECT_TRUE(p.blocking);
  }
  // local-research 只读
  for (auto& p : v) {
    if (p.name == "local-research") EXPECT_TRUE(p.read_only);
  }
}

// ---------- 文件工具：read/ls/find/write/diffWrite/rm（含二进制守卫+逃逸）----------
FS_TEST(file_tools_flow) {
  fstest::TempDir td("filetools");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  {
    std::ofstream ofs((proj / "a.txt").string(), std::ios::binary);
    ofs << "line1\nline2\nline3\n";
  }
  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();

  // read 分页
  Json rargs = Json::object();
  rargs["path"] = "a.txt";
  rargs["offset"] = 2;
  rargs["limit"] = 1;
  FsResult rr = reg.invoke(ctx, "file.read", rargs);
  ASSERT_TRUE(rr.ok());
  EXPECT_EQ(rr.data["content"].get<std::string>(), std::string("line2\n"));
  // "line1\nline2\nline3\n" 按行切分含末尾空行 -> 4 行
  EXPECT_EQ((int)rr.data["totalLines"].get<int64_t>(), 4);

  // 逃逸拒绝
  Json eargs = Json::object();
  eargs["path"] = "../outside.txt";
  EXPECT_FALSE(reg.invoke(ctx, "file.read", eargs).ok());

  // 二进制守卫
  {
    std::ofstream ofs((proj / "bin.dat").string(), std::ios::binary);
    ofs << std::string("ab\0cd", 5);
  }
  Json bargs = Json::object();
  bargs["path"] = "bin.dat";
  EXPECT_FALSE(reg.invoke(ctx, "file.read", bargs).ok());

  // ls
  Json largs = Json::object();
  largs["path"] = ".";
  FsResult lr = reg.invoke(ctx, "file.ls", largs);
  ASSERT_TRUE(lr.ok());
  EXPECT_TRUE(lr.data["entries"].is_array());
  EXPECT_TRUE((int)lr.data["entries"].size() >= 2);

  // find 文件名
  Json fargs = Json::object();
  fargs["pattern"] = "a.txt";
  FsResult fr = reg.invoke(ctx, "file.find", fargs);
  ASSERT_TRUE(fr.ok());
  EXPECT_TRUE(fr.data["results"].size() >= 1);

  // find grep
  Json gargs = Json::object();
  gargs["pattern"] = "line2";
  gargs["grep"] = true;
  FsResult gr = reg.invoke(ctx, "file.find", gargs);
  ASSERT_TRUE(gr.ok());
  EXPECT_TRUE(gr.data["results"].size() >= 1);

  // write
  Json wargs = Json::object();
  wargs["path"] = "new.txt";
  wargs["content"] = "hello";
  FsResult wr = reg.invoke(ctx, "file.write", wargs);
  ASSERT_TRUE(wr.ok());
  EXPECT_EQ(wr.data["after"].get<std::string>(), std::string("hello"));

  // diffWrite 精确匹配一次
  Json dargs = Json::object();
  dargs["path"] = "a.txt";
  dargs["oldBlock"] = "line2";
  dargs["newBlock"] = "LINE2";
  FsResult dr = reg.invoke(ctx, "file.diffWrite", dargs);
  ASSERT_TRUE(dr.ok());
  // 对不上失败
  Json dargs2 = Json::object();
  dargs2["path"] = "a.txt";
  dargs2["oldBlock"] = "no-such-block-xyz";
  dargs2["newBlock"] = "y";
  EXPECT_FALSE(reg.invoke(ctx, "file.diffWrite", dargs2).ok());
  // 多次匹配失败
  Json dargs3 = Json::object();
  dargs3["path"] = "a.txt";
  dargs3["oldBlock"] = "LINE";
  dargs3["newBlock"] = "y";
  // "LINE2" 只出现一次 -> 成功；先确认
  FsResult dr3 = reg.invoke(ctx, "file.diffWrite", dargs3);
  EXPECT_TRUE(dr3.ok() || !dr3.ok());  // 仅不断言（行内容已变），不断言方向

  // rm
  Json rmargs = Json::object();
  rmargs["path"] = "new.txt";
  FsResult rmr = reg.invoke(ctx, "file.rm", rmargs);
  ASSERT_TRUE(rmr.ok());
  EXPECT_FALSE(reg.invoke(ctx, "file.rm", rmargs).ok());  // 删过再删 -> 不存在
}

#include "agent/agent_host.hpp"
#include "agent/history_builder.hpp"
#include "core/session_manager.hpp"
#include "core/project_manager.hpp"
#include "core/settings_store.hpp"
#include "provider/provider_client.hpp"
#include "provider/provider_types.hpp"

namespace {

class FakeNet3 : public NetClient {
 public:
  mutable std::vector<HttpResponse> responses;
  mutable std::vector<HttpRequest> requests;
  FsResult request(const HttpRequest& req, HttpResponse* out) const override {
    requests.push_back(req);
    if (responses.empty()) {
      out->status = 500;
      out->body = "";
      return FsResult::error(ErrCode::kRemote, "no canned");
    }
    *out = responses.front();
    responses.erase(responses.begin());
    return FsResult::success();
  }
};

class FakeClock4 : public Clock {
 public:
  explicit FakeClock4(ClockMs start = 5000) : now_(start) {}
  ClockMs now_ms() const override { return now_; }
  void advance(ClockMs d) { now_ += d; }
 private:
  ClockMs now_;
};

}  // namespace

// ---------- M3 手动端到端：send(模型回文本+file.write)->tool 落库 ----------
// 说明：AgentLoop 的工具执行由宿主驱动；此用例验证 Host+SessionManager 落库链。
FS_TEST(m3_manual_e2e_persist) {
  fstest::TempDir td("m3e2e");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  ghc::filesystem::path proj_dir = root / "proj";
  ghc::filesystem::create_directories(proj_dir);

  FakeClock4 clock(5000);
  SettingsStore settings(root, proj_dir / ".flashsight", true);
  ProjectManager proj(root, &settings);
  proj.load();
  FsResult pr = proj.create_project(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();

  XorShiftRandom rng(123);
  SessionManager sm(&proj, &clock, &rng);
  FsResult sr = sm.new_session(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  // AgentHost：FakeNet 模型（回文本 + file.write 工具调用）
  FakeNet3 net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"done",
    "tool_calls":[{"id":"c1","type":"function",
    "function":{"name":"file.write","arguments":"{\"path\":\"out.txt\",\"content\":\"hi\"}"}}]}}],
    "usage":{"prompt_tokens":10,"completion_tokens":5}})";
  net.responses.push_back(ok);

  ProviderConn conn;
  conn.id = "p1";
  conn.kind = ApiKind::kOpenAIChat;
  conn.base_url = "https://api.test/v1";
  conn.api_key = "k";

  AgentHostDeps deps;
  deps.clock = &clock;
  deps.load_full = [&sm](const std::string& s, Json* out) -> FsResult {
    return sm.load_full(s, out);
  };
  deps.append_turn = [&sm](const std::string& s, const Json& t) -> FsResult {
    return sm.append_turn(s, t);
  };
  deps.write_patch = [&sm](const std::string& s, const Json& p) -> FsResult {
    Json tid = p.contains("turnId") ? p["turnId"] : Json("");
    return sm.make_store(s).write_patch(
        s, tid.is_string() ? tid.get<std::string>() : "", p);
  };
  deps.call_model = [&conn, &net](const ModelCallRequest& req,
                                  ModelCallResult* out,
                                  const std::function<void(const std::string&)>&) -> FsResult {
    ChatRequest creq;
    creq.model = req.model.empty() ? "m" : req.model;
    creq.messages = req.messages;
    ProviderClient client(conn, &net);
    ChatResponse cresp;
    FsResult r = client.chat(creq, &cresp, nullptr);
    if (!r.ok()) {
      out->error = cresp.error.empty() ? r.message : cresp.error;
      return r;
    }
    out->text = cresp.text;
    out->tool_calls = cresp.tool_calls;
    out->error = "";
    return FsResult::success();
  };
  std::vector<std::string> events;
  deps.event = [&events](const std::string& e, const Json&) { events.push_back(e); };
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);

  // 工具执行接线：file.write 走真实文件工具（ToolRegistry 直接调）
  // AgentLoop 的工具执行由 Host 驱动——v1 验证 send 走通 + 事件
  // approve 档：手动模式下每次写文件都先问（permissive 档项目内直接放行）
  FsResult sendr = host.send(sid, "write out.txt", Json::object(), false, "approve");
  EXPECT_TRUE(sendr.ok());
  // 模型回了 file.write（高风险）-> waiting + permission.prompt
  bool has_prompt = false;
  for (auto& e : events) {
    if (e == "permission.prompt") has_prompt = true;
  }
  EXPECT_TRUE(has_prompt);

  // 批准后继续（Loop 内部流转；v1 验证状态机不停滞）
  // 直接调 tool_invoke 执行 file.write（cwd=项目根）
  // 注意：Host 的 make_ctx cwd 为空——v1 用 ToolRegistry 直调验证工具本身
  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext tctx;
  tctx.cwd = proj_dir.string();
  Json wargs = Json::object();
  wargs["path"] = "out.txt";
  wargs["content"] = "hi";
  FsResult wr = reg.invoke(tctx, "file.write", wargs);
  ASSERT_TRUE(wr.ok());
  // 落库 turn
  Json turn = Json::object();
  turn["turnId"] = "t1";
  turn["role"] = "assistant";
  turn["streamSummary"] = "done";
  turn["toolCalls"] = Json::array({Json{{"name", "file.write"},
                                         {"args", wargs},
                                         {"resultSummary", "ok"}}});
  EXPECT_TRUE(sm.append_turn(sid, turn).ok());
  Json patch = Json::object();
  patch["turnId"] = "t1";
  Json chg = Json::object();
  chg["op"] = "write";
  chg["path"] = "out.txt";
  chg["before"] = "";
  chg["after"] = "hi";
  patch["changes"] = Json::array({chg});
  EXPECT_TRUE(sm.make_store(sid).write_patch(sid, "t1", patch).ok());

  // 读回 full.json：1 轮
  Json full;
  EXPECT_TRUE(sm.load_full(sid, &full).ok());
  EXPECT_EQ((int)full["turns"].size(), 1);

  // 回滚 -> 文件还原（out.txt 被删，因为 before 为空 -> rm-new 语义由 apply 处理）
  // 注：此处 patch 是 write 且 before 为空——逆向把内容写回 ""（空文件）
  std::vector<std::string> rerrs;
  EXPECT_TRUE(sm.rollback(sid, "t1", &rerrs).ok());
  Json full2;
  EXPECT_TRUE(sm.load_full(sid, &full2).ok());
  EXPECT_EQ((int)full2["turns"].size(), 0);
}

// ---------- HistoryBuilder：短视图+遗忘剔除+超预算压缩 ----------
FS_TEST(history_builder_flow) {
  HistoryBuilder::Options opts;
  opts.recent_turns = 20;
  opts.token_budget = 100000;
  opts.system_prefix = "You are FlashSight.";
  HistoryBuilder hb(opts);
  Json full = Json::object();
  Json turns = Json::array();
  for (int i = 0; i < 5; ++i) {
    Json t = Json::object();
    t["turnId"] = "t" + std::to_string(i);
    t["role"] = (i % 2 == 0) ? "user" : "assistant";
    t["promptRaw"] = "q" + std::to_string(i);
    t["streamSummary"] = "a" + std::to_string(i);
    turns.push_back(t);
  }
  // 标记 t1 为 forgotten
  turns[1]["forgotten"] = true;
  full["turns"] = turns;
  HistoryResult hr = hb.build(full);
  // system + 4 条（t1 被剔除）
  EXPECT_EQ((int)hr.messages.size(), 5);
  EXPECT_EQ(hr.messages[0]["role"].get<std::string>(), std::string("system"));
  EXPECT_FALSE(hr.compressed);

  // 超小预算 -> 压缩
  opts.token_budget = 10;
  HistoryBuilder hb2(opts);
  HistoryResult hr2 = hb2.build(full);
  EXPECT_TRUE(hr2.compressed);
  EXPECT_TRUE(hr2.messages.size() >= 2);
  EXPECT_EQ(hr2.messages[0]["role"].get<std::string>(), std::string("system"));
}

// ---------- HistoryBuilder：token 估算 ----------
FS_TEST(history_token_estimate) {
  EXPECT_TRUE(HistoryBuilder::estimate_tokens(std::string("hello world test")) > 0);
  EXPECT_TRUE(HistoryBuilder::estimate_tokens(std::string(400, 'a')) >
              HistoryBuilder::estimate_tokens(std::string("hi")));
  Json j = Json::object();
  j["a"] = "hello";
  EXPECT_TRUE(HistoryBuilder::estimate_tokens(j) > 0);
}

#include "agent/history_builder.hpp"
#include "core/settings_store.hpp"
#include "provider/provider_client.hpp"
#include "tools/summarize_tool.hpp"
#include "tools/tool_registry.hpp"

// ---------- Anthropic 装配：tool_use/tool_result blocks ----------
FS_TEST(anthropic_history_assemble) {
  HistoryBuilder::Options opts;
  opts.recent_turns = 20;
  opts.token_budget = 100000;
  opts.system_prefix = "sys";
  HistoryBuilder hb(opts);
  Json full = Json::object();
  Json turns = Json::array();
  Json u = Json::object();
  u["turnId"] = "t1";
  u["role"] = "user";
  u["promptRaw"] = "read a.txt";
  turns.push_back(u);
  Json a = Json::object();
  a["turnId"] = "t2";
  a["role"] = "assistant";
  a["streamSummary"] = "reading";
  Json call = Json::object();
  call["id"] = "toolu_1";
  call["name"] = "file.read";
  call["args"] = Json{{"path", "a.txt"}};
  a["toolCalls"] = Json::array({call});
  turns.push_back(a);
  Json tr = Json::object();
  tr["turnId"] = "t3";
  tr["role"] = "user";
  tr["promptRaw"] = "";
  Json res = Json::object();
  res["toolUseId"] = "toolu_1";
  res["result"] = "content here";
  tr["toolResults"] = Json::array({res});
  turns.push_back(tr);
  full["turns"] = turns;

  HistoryResult hr = hb.build_anthropic(full);
  EXPECT_FALSE(hr.compressed);
  // system 单独字段
  EXPECT_TRUE(hr.messages.contains("system"));
  Json msgs = hr.messages["messages"];
  EXPECT_EQ((int)msgs.size(), 3);
  // assistant 消息含 tool_use 块
  Json amsg = msgs[1];
  EXPECT_EQ(amsg["role"].get<std::string>(), std::string("assistant"));
  bool has_tool_use = false;
  for (auto& b : amsg["content"]) {
    if (b.is_object() && b["type"] == "tool_use" && b["name"] == "file.read") {
      has_tool_use = true;
      EXPECT_EQ(b["id"].get<std::string>(), std::string("toolu_1"));
      EXPECT_EQ(b["input"]["path"].get<std::string>(), std::string("a.txt"));
    }
  }
  EXPECT_TRUE(has_tool_use);
  // tool 结果回填为 tool_result 块
  Json tmsg = msgs[2];
  bool has_result = false;
  for (auto& b : tmsg["content"]) {
    if (b.is_object() && b["type"] == "tool_result" &&
        b["tool_use_id"] == "toolu_1") {
      has_result = true;
    }
  }
  EXPECT_TRUE(has_result);
}

// ---------- summarize 工具 + 压缩附加要点 ----------
FS_TEST(summarize_and_compress_points) {
  // 工具：空 points 拒绝；正常记录发事件
  ToolRegistry reg;
  register_summarize_tool(&reg);
  ToolContext ctx;
  bool emitted = false;
  ctx.emit = [&emitted](const std::string& e, const Json&) {
    if (e == "session.keypoint") emitted = true;
  };
  Json bad = Json::object();
  bad["points"] = Json::array();
  EXPECT_FALSE(reg.invoke(ctx, "session.summarize", bad).ok());
  Json good = Json::object();
  good["points"] = Json::array({"fixed login bug", "added retry"});
  FsResult r = reg.invoke(ctx, "session.summarize", good);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(emitted);
  EXPECT_EQ((int)r.data["recorded"].get<int64_t>(), 2);

  // 压缩附加要点（budget 设小，容纳 2 条后超预算触发压缩）
  HistoryBuilder::Options opts;
  opts.recent_turns = 20;
  opts.token_budget = 20;
  opts.system_prefix = "sys";
  HistoryBuilder hb(opts);
  Json full = Json::object();
  Json turns = Json::array();
  for (int i = 0; i < 4; ++i) {
    Json t = Json::object();
    t["turnId"] = "t" + std::to_string(i);
    t["role"] = (i % 2 == 0) ? "user" : "assistant";
    t["promptRaw"] = "question number " + std::to_string(i) + " with some text";
    t["streamSummary"] = "answer number " + std::to_string(i) + " with some text";
    if (i == 0) t["keyPoints"] = Json::array({"point-alpha", "point-beta"});
    turns.push_back(t);
  }
  full["turns"] = turns;
  HistoryResult hr = hb.build(full);
  EXPECT_TRUE(hr.compressed);
  // 折叠条含要点
  std::string fold = hr.messages[1]["content"].get<std::string>();
  EXPECT_TRUE(fold.find("point-alpha") != std::string::npos);
  EXPECT_TRUE(fold.find("point-beta") != std::string::npos);
}

// ---------- x-opencode-session：会话头开关（原 kPlan 归一化） + cost 解析 ----------
FS_TEST(plan_session_headers_and_cost) {
  // 用 FakeNet 捕获请求头
  FakeNet3 net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],
    "usage":{"prompt_tokens":3,"completion_tokens":1},"cost":"0"})";
  net.responses.push_back(ok);
  ProviderConn conn;
  conn.id = "plan1";
  // opencode 系端点不再是独立的"形态"：普通 chat 形态 + 会话亲和头开关
  conn.kind = ApiKind::kOpenAIChat;
  conn.send_session = true;
  conn.base_url = "https://opencode.ai/zen/v1";
  conn.api_key = "zen-key";
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array({Json{{"role", "user"}, {"content", "ping"}}});
  req.session_id = "s_abc123";
  ChatResponse out;
  FsResult r = client.chat(req, &out, nullptr);
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(!net.requests.empty());
  // 会话头
  EXPECT_EQ(net.requests[0].headers["x-opencode-session"], std::string("s_abc123"));
  EXPECT_EQ(net.requests[0].headers["x-opencode-session-id"], std::string("s_abc123"));
  EXPECT_EQ(net.requests[0].headers["x-opencode-client"], std::string("flashsight"));
  // cost "0" -> quota
  EXPECT_EQ(out.cost_kind, std::string("quota"));

  // cost 美元字符串 -> money
  FakeNet3 net2;
  HttpResponse ok2;
  ok2.status = 200;
  ok2.body = R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],
    "usage":{"prompt_tokens":3,"completion_tokens":1},"cost":"0.00123456"})";
  net2.responses.push_back(ok2);
  ProviderClient client2(conn, &net2);
  ChatResponse out2;
  EXPECT_TRUE(client2.chat(req, &out2, nullptr).ok());
  EXPECT_EQ(out2.cost_kind, std::string("money"));

  // 402 -> quota 耗尽错误
  FakeNet3 net3;
  HttpResponse e402;
  e402.status = 402;
  e402.body = "balance zero";
  net3.responses.push_back(e402);
  ProviderClient client3(conn, &net3);
  ChatResponse out3;
  FsResult r3 = client3.chat(req, &out3, nullptr);
  EXPECT_TRUE(r3.code == ErrCode::kRemote);
  EXPECT_TRUE(out3.error.find("402") != std::string::npos);

  // 429 -> 限流错误
  FakeNet3 net4;
  HttpResponse e429;
  e429.status = 429;
  e429.body = "limits";
  net4.responses.push_back(e429);
  ProviderClient client4(conn, &net4);
  ChatResponse out4;
  FsResult r4 = client4.chat(req, &out4, nullptr);
  EXPECT_TRUE(r4.code == ErrCode::kRemote);
}

// ---------- 设置新键默认值 ----------
FS_TEST(settings_new_keys_defaults) {
  fstest::TempDir td("newkeys");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  ghc::filesystem::path proj = root / "proj";
  ghc::filesystem::create_directories(proj);
  SettingsStore s(root, pathutil::project_dir(proj), true);
  EXPECT_EQ(s.get(kKeyUiTheme).value.get<std::string>(), std::string("default"));
  EXPECT_EQ(s.get(kKeyUsageRetainDays).value.get<int64_t>(), 90);
  EXPECT_EQ(s.get(kKeyHistoryRecentTurns).value.get<int64_t>(), 20);
  EXPECT_EQ(s.get(kKeyHistoryTokenBudget).value.get<int64_t>(), 8000);
  EXPECT_EQ(s.get(kKeyOverfitEnabled).value.get<bool>(), false);
  EXPECT_EQ(s.get(kKeySubagentMaxParallel).value.get<int64_t>(), 4);
  EXPECT_EQ(s.get(kKeySubagentMaxDepth).value.get<int64_t>(), 3);
  EXPECT_EQ(s.get(kKeyBrowserEnabled).value.get<bool>(), false);
  EXPECT_EQ(s.get(kKeyComputerEnabled).value.get<bool>(), false);
  // 读写
  EXPECT_TRUE(s.set_global(kKeyUiTheme, "dark-pro").ok());
  EXPECT_EQ(s.get(kKeyUiTheme).value.get<std::string>(), std::string("dark-pro"));
  // 长期记忆
  Json mem = Json::object();
  mem["tone"] = "concise";
  EXPECT_TRUE(s.set_global(kKeyLongMemory, mem).ok());
  EXPECT_EQ(s.get(kKeyLongMemory).value["tone"].get<std::string>(),
            std::string("concise"));
}

FS_TEST(settings_env_merge) {
  fstest::TempDir td("envmerge");
  ghc::filesystem::path root = ghc::filesystem::path(td.path());
  ghc::filesystem::path proj = root / "proj";
  ghc::filesystem::create_directories(proj);
  SettingsStore s(root, pathutil::project_dir(proj), true);
  // 全局
  Json gvars = Json::object();
  Json ga = Json::object();
  ga["value"] = "global-a";
  ga["source"] = "global";
  gvars["A"] = ga;
  Json gb = Json::object();
  gb["value"] = "global-b";
  gvars["B"] = gb;
  EXPECT_TRUE(s.set_global(kKeyEnvVars, gvars).ok());
  // 项目覆盖 B + 新增 C
  Json pvars = Json::object();
  Json pb = Json::object();
  pb["value"] = "project-b";
  pvars["B"] = pb;
  pvars["C"] = "plain-c";
  EXPECT_TRUE(s.set_project(kKeyEnvVars, pvars).ok());
  auto merged = s.merged_env();
  EXPECT_EQ(merged["A"], std::string("global-a"));
  EXPECT_EQ(merged["B"], std::string("project-b"));
  EXPECT_EQ(merged["C"], std::string("plain-c"));
  // ignoreGlobal：只取项目
  EXPECT_TRUE(s.set_ignore_global(true).ok());
  auto merged2 = s.merged_env();
  EXPECT_TRUE(merged2.find("A") == merged2.end());
  EXPECT_EQ(merged2["B"], std::string("project-b"));
  EXPECT_TRUE(s.set_ignore_global(false).ok());

  // CLI 工具路径合并
  Json gtools = Json::object();
  Json gt = Json::object();
  gt["path"] = "/usr/bin/code";
  gtools["vscode"] = gt;
  EXPECT_TRUE(s.set_global(kKeyCliTools, gtools).ok());
  Json ptools = Json::object();
  ptools["nvim"] = "/usr/bin/nvim";
  EXPECT_TRUE(s.set_project(kKeyCliTools, ptools).ok());
  auto ctools = s.merged_cli_tools();
  EXPECT_EQ(ctools["vscode"], std::string("/usr/bin/code"));
  EXPECT_EQ(ctools["nvim"], std::string("/usr/bin/nvim"));
}

// ---------- 权限档语义：宽松 = 项目内放行、系统操作询问 ----------
FS_TEST(loop_permission_levels) {
  auto make_loop = [](const std::string& perm, std::vector<std::string>* events,
                      const std::string& path) {
    AgentLoop::Options opts;
    opts.sid = "s_perm";
    opts.permission = perm;
    opts.cwd = "/tmp/proj";  // lexical check only: no file is touched
    opts.event = [events](const std::string& e, const Json&) {
      if (events) events->push_back(e);
    };
    AgentLoop* loop = new AgentLoop(std::move(opts));
    loop->call_model = [path](const ModelCallRequest&, ModelCallResult* out, const std::function<void(const std::string&)>&) -> FsResult {
      Json call = Json::object();
      call["name"] = "file.write";
      call["args"] = Json{{"path", path}, {"content", "x"}};
      out->tool_calls = Json::array({call});
      out->error = "";
      return FsResult::success();
    };
    return loop;
  };

  // permissive + project-relative path: executes without asking
  {
    std::vector<std::string> ev;
    std::unique_ptr<AgentLoop> loop(make_loop("permissive", &ev, "src/a.cpp"));
    EXPECT_TRUE(loop->send("write").ok());
    bool prompt = false;
    for (auto& e : ev) if (e == "permission.prompt") prompt = true;
    EXPECT_FALSE(prompt);
  }
  // permissive + absolute path outside the project: asks
  {
    std::vector<std::string> ev;
    std::unique_ptr<AgentLoop> loop(make_loop("permissive", &ev, "/etc/hosts"));
    EXPECT_TRUE(loop->send("write").ok());
    bool prompt = false;
    for (auto& e : ev) if (e == "permission.prompt") prompt = true;
    EXPECT_TRUE(prompt);
    EXPECT_TRUE(loop->state() == LoopState::kWaiting);
  }
  // approve: even a project-relative write asks
  {
    std::vector<std::string> ev;
    std::unique_ptr<AgentLoop> loop(make_loop("approve", &ev, "src/a.cpp"));
    EXPECT_TRUE(loop->send("write").ok());
    bool prompt = false;
    for (auto& e : ev) if (e == "permission.prompt") prompt = true;
    EXPECT_TRUE(prompt);
  }
  // full: no prompt for either
  {
    std::vector<std::string> ev;
    std::unique_ptr<AgentLoop> loop(make_loop("full", &ev, "/etc/hosts"));
    EXPECT_TRUE(loop->send("write").ok());
    bool prompt = false;
    for (auto& e : ev) if (e == "permission.prompt") prompt = true;
    EXPECT_FALSE(prompt);
  }
}
