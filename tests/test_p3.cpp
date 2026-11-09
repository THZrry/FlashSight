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
#include "common/fs_json.hpp"
#include "common/fs_path.hpp"
#include "common/fs_utf8.hpp"

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
    call["name"] = "write_file";
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
  // approve 档：remove_file 也要先问（宽松档下项目内的删除是直接放行的）
  opts.permission = "approve";
  opts.event = [](const std::string&, const Json&) {};
  AgentLoop loop(std::move(opts));
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out, const std::function<void(const std::string&)>&) -> FsResult {
    ++calls;
    if (calls == 1) {
      Json call = Json::object();
      call["name"] = "remove_file";
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

// ---------- Loop：工具调用全链路（start → 执行 → update → 落库） ----------
// The UI shows a tool the moment it starts and fills in its result afterwards.
// That only works if the loop emits tool.start with the args, tool.update with
// a summary + detail, and persists what actually RAN (not just what the model
// asked for) - a reloaded session otherwise had rows with nothing to expand.
FS_TEST(loop_tool_call_events_and_persist) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  opts.permission = "full";   // no approval gate: execute immediately
  std::vector<std::string> events;
  Json start_payload, update_payload, persisted;
  opts.event = [&](const std::string& e, const Json& p) {
    events.push_back(e);
    if (e == "tool.start") start_payload = p;
    if (e == "tool.update") update_payload = p;
  };
  AgentLoop loop(std::move(opts));
  loop.persist_turn = [&persisted](const Json& t) {
    if (jsonutil::get<std::string>(t, "role", "") == "assistant") persisted = t;
  };
  loop.invoke_tool = [](const ToolContext&, const std::string& name, const Json&) {
    return FsResult::success(Json{{"summary", "42 lines"}, {"content", "x"}});
  };
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out,
                             const std::function<void(const std::string&)>&) -> FsResult {
    ++calls;
    if (calls == 1) {
      Json call = Json::object();
      call["name"] = "read_file";
      call["id"] = "call_1";
      call["args"] = Json{{"path", "main.cpp"}};
      out->tool_calls = Json::array({call});
    } else {
      out->text = "done";
    }
    return FsResult::success();
  };
  EXPECT_TRUE(loop.send("read it").ok());

  bool saw_start = false, saw_update = false;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i] == "tool.start") saw_start = true;
    if (events[i] == "tool.update") saw_update = true;
  }
  EXPECT_TRUE(saw_start);
  EXPECT_TRUE(saw_update);
  // start carries the arguments (so the row can say WHAT is running)...
  EXPECT_EQ(jsonutil::get<std::string>(start_payload, "name", ""),
            std::string("read_file"));
  EXPECT_TRUE(start_payload.contains("args"));
  // ...update carries the verdict and a one-line summary.
  EXPECT_TRUE(jsonutil::get<bool>(update_payload, "ok", false));
  EXPECT_EQ(jsonutil::get<std::string>(update_payload, "summary", ""),
            std::string("42 lines"));
  EXPECT_TRUE(update_payload.contains("detail"));

  // The stored assistant turn keeps what ran, so a reload shows the same rows.
  EXPECT_TRUE(persisted.contains("toolCalls"));
  EXPECT_EQ(persisted["toolCalls"].size(), static_cast<size_t>(1));
  EXPECT_EQ(jsonutil::get<std::string>(persisted["toolCalls"][0], "summary", ""),
            std::string("42 lines"));
  EXPECT_TRUE(jsonutil::get<bool>(persisted["toolCalls"][0], "ok", false));
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
    call["name"] = "read_file";
    call["args"] = Json{{"path", "a.txt"}};
    out->tool_calls = Json::array({call});
    out->error = "";
    return FsResult::success();
  };
  loop.suspend();
  EXPECT_TRUE(loop.send("go").ok());
  // read_file 非高风险 -> 直接执行回填，不再发第二轮（suspend 后停）
  EXPECT_TRUE(calls >= 1);
  bool has_suspended = false;
  for (auto& e : events) {
    if (e == "loop.state") has_suspended = true;
  }
  EXPECT_TRUE(has_suspended);
  loop.resume();
}

// 挂起 = GRACEFUL. Pressed while a model call is in flight, it must NOT cut the
// round: the reply finishes, the tools it asked for run, and the loop parks
// before the next model call. (0.46: the old code aborted the transfer here,
// which is why it looked like the turn was interrupted.)
FS_TEST(loop_suspend_finishes_round_then_parks) {
  AgentLoop::Options opts;
  opts.sid = "s_g";
  AgentLoop loop(std::move(opts));
  AgentLoop* lp = &loop;
  int calls = 0;
  int tools = 0;
  loop.call_model = [lp, &calls](const ModelCallRequest&, ModelCallResult* out,
                                 const std::function<void(const std::string&)>&) -> FsResult {
    ++calls;
    if (calls == 1) {
      out->text = "round one";
      Json call = Json::object();
      call["name"] = "read_file";
      call["id"] = "call_g1";
      call["args"] = Json{{"path", "a.txt"}};
      out->tool_calls = Json::array({call});
      lp->suspend();  // user pressed 挂起 while this call was running
      return FsResult::success();
    }
    out->text = "second round";
    return FsResult::success();
  };
  loop.invoke_tool = [&tools](const ToolContext&, const std::string&,
                              const Json&) {
    ++tools;
    return FsResult::success(Json{{"ok", true}});
  };
  EXPECT_TRUE(loop.send("go").ok());
  EXPECT_EQ(calls, 1);   // no second model call
  EXPECT_EQ(tools, 1);   // but this round's tool DID run
  EXPECT_TRUE(loop.parked());
  // Resume continues without re-asking the same question.
  EXPECT_TRUE(loop.resume_task().ok());
  EXPECT_EQ(calls, 2);
  EXPECT_FALSE(loop.parked());
}

// ---------- Loop：停止生成必须真的停住，并且"恢复"必须真的继续 ----------
//
// Two things used to be missing. (1) suspend only took effect between tool
// batches, so a plain streaming answer ran to completion no matter how often the
// user pressed 停止 - the abort predicate now cuts the transfer. (2) resume()
// only cleared the request flag; once the loop HAD parked it was idle, nothing was
// left to notice the flag, so 恢复 did nothing ("挂起功能疑似没做全").
FS_TEST(loop_resume_task_continues_parked_turn) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  std::vector<std::string> events;
  opts.event = [&events](const std::string& e, const Json&) { events.push_back(e); };
  AgentLoop loop(std::move(opts));
  AgentLoop* lp = &loop;
  int calls = 0;
  bool saw_partial = false;
  loop.call_model = [lp, &calls, &saw_partial](
                        const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>& on_delta)
      -> FsResult {
    ++calls;
    if (calls == 1) {
      // The model is mid-answer when the user presses 中止 (the HARD stop -
      // 挂起 is graceful and would let this round finish instead).
      out->text = "half an ans";
      if (on_delta) on_delta("half an ans");
      lp->interrupt();
      return FsResult::error(ErrCode::kCancelled, "stopped by user");
    }
    // The resumed call must still carry what the model already said, otherwise the
    // user's "continue" asks the model to answer the same question from scratch.
    for (size_t i = 0; i < req.messages.size(); ++i) {
      std::string role = jsonutil::get<std::string>(req.messages[i], "role", "");
      std::string content = jsonutil::get<std::string>(req.messages[i], "content", "");
      if (role == "assistant" && content.find("stopped by user") != std::string::npos) {
        saw_partial = true;
      }
    }
    out->text = "done";
    out->error = "";
    return FsResult::success();
  };
  EXPECT_TRUE(loop.send("go").ok());
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(loop.parked());
  EXPECT_EQ(std::string(loop_state_str(loop.state())), std::string("idle"));
  // resume_task re-runs the SAVED transcript - no extra user message, because the
  // user asked to continue the same task, not to say something new.
  EXPECT_TRUE(loop.resume_task().ok());
  EXPECT_FALSE(loop.parked());
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(saw_partial);
  EXPECT_EQ(std::string(loop_state_str(loop.state())), std::string("turnDone"));
}

// A message sent while the turn is stopped/suspended CONTINUES it with that text
// as guidance, instead of starting a new turn that drops the parked transcript.
FS_TEST(loop_send_while_parked_continues) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  AgentLoop loop(std::move(opts));
  AgentLoop* lp = &loop;
  int calls = 0;
  bool guidance_seen = false;
  loop.call_model = [lp, &calls, &guidance_seen](
                        const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>& on_delta)
      -> FsResult {
    ++calls;
    if (calls == 1) {
      out->text = "working";
      if (on_delta) on_delta("working");
      lp->interrupt();  // 中止 (hard stop); 挂起 would finish the round first
      return FsResult::error(ErrCode::kCancelled, "stopped by user");
    }
    for (size_t i = 0; i < req.messages.size(); ++i) {
      if (jsonutil::get<std::string>(req.messages[i], "role", "") == "user" &&
          jsonutil::get<std::string>(req.messages[i], "content", "")
                  .find("also check b.txt") != std::string::npos) {
        guidance_seen = true;
      }
    }
    out->text = "ok";
    out->error = "";
    return FsResult::success();
  };
  EXPECT_TRUE(loop.send("go").ok());
  EXPECT_TRUE(loop.parked());
  EXPECT_TRUE(loop.send("also check b.txt").ok());
  EXPECT_EQ(calls, 2);
  EXPECT_TRUE(guidance_seen);
  EXPECT_FALSE(loop.parked());
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

// The queue is rendered above the composer; unbounded growth pushed the input
// out of the window. Reject the (kMax+1)-th item instead of silently evicting
// an older instruction.
FS_TEST(steering_queue_is_bounded) {
  SteeringQueue q;
  for (size_t i = 0; i < kMaxSteeringItems; ++i) {
    EXPECT_TRUE(q.push(SteeringKind::kRoundEnd, "item") > 0);
  }
  EXPECT_EQ(q.push(SteeringKind::kRoundEnd, "overflow"), 0);
  EXPECT_EQ(static_cast<int>(q.items().size()),
            static_cast<int>(kMaxSteeringItems));
}

// The new steering contract: the NEXT call carries the WHOLE queue; "send now"
// takes exactly ONE item (the other queued items stay).
FS_TEST(steering_queue_drain_all_and_take_one) {
  SteeringQueue q;
  int a = q.push(SteeringKind::kRoundEnd, "one");
  int b = q.push(SteeringKind::kRoundEnd, "two");
  std::string taken;
  EXPECT_TRUE(q.take(a, &taken));
  EXPECT_EQ(taken, std::string("one"));
  EXPECT_EQ((int)q.items().size(), 1);
  EXPECT_EQ(q.drain_all(), std::string("two"));
  EXPECT_TRUE(q.empty());
  EXPECT_FALSE(q.take(b, &taken));
}

#include "agent/overfit.hpp"
#include "agent/subagent.hpp"
#include "common/fs_simhash.hpp"
#include "tools/file_tools.hpp"
#include "tools/tool_registry.hpp"
#include "tools/ask_tool.hpp"


// 继续 (empty guidance) re-runs the SAVED transcript without inserting an empty
// user message. The UI's 继续 button sends an empty guidance on purpose; the old
// code appended "" as a user turn, which made the button look dead.
FS_TEST(loop_continue_with_empty_guidance) {
  AgentLoop::Options opts;
  opts.sid = "s_cont";
  AgentLoop loop(std::move(opts));
  int calls = 0;
  bool saw_empty_user = false;
  loop.call_model = [&calls, &saw_empty_user](
                        const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>&) -> FsResult {
    ++calls;
    for (size_t i = 0; i < req.messages.size(); ++i) {
      if (jsonutil::get<std::string>(req.messages[i], "role", "") == "user" &&
          jsonutil::get<std::string>(req.messages[i], "content", "").empty()) {
        saw_empty_user = true;
      }
    }
    if (calls == 1) {
      out->error = "provider blew up";
      return FsResult::success();
    }
    out->text = "recovered";
    return FsResult::success();
  };
  loop.send("go");
  EXPECT_EQ(std::string(loop_state_str(loop.state())), std::string("turnDone"));
  EXPECT_TRUE(loop.continue_task("").ok());
  EXPECT_EQ(calls, 2);
  EXPECT_FALSE(saw_empty_user);
  EXPECT_TRUE(std::string(loop_state_str(loop.state())) == std::string("turnDone"));
}


// A range edit does not compare old text. CRLF/LF differences therefore cannot
// reject a valid edit; only the file hash is checked.
FS_TEST(edit_file_range_replaces_regardless_of_old_text) {
  fstest::TempDir td("editeol");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  {
    std::ofstream ofs((proj / "crlf.txt").string(), std::ios::binary);
    ofs << "alpha\r\nbeta\r\ngamma\r\n";
  }
  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();
  FsResult rr = reg.invoke(ctx, "read_file", Json{{"path", "crlf.txt"}});
  ASSERT_TRUE(rr.ok());
  FsResult er = reg.invoke(
      ctx, "edit_file",
      Json{{"path", "crlf.txt"},
           {"hash", rr.data["hash"]},
           {"edits", Json::array({Json{{"start", 2}, {"end", 2}, {"newText", "BETA"}}})}});
  ASSERT_TRUE(er.ok());
  FsResult r2 = reg.invoke(ctx, "read_file", Json{{"path", "crlf.txt"}});
  ASSERT_TRUE(r2.ok());
  std::string out = r2.data["content"].get<std::string>();
  EXPECT_TRUE(out.find("BETA") != std::string::npos);
  EXPECT_TRUE(out.find('\r') == std::string::npos);
}

// STRICT edit protocol: {start,end,newText} exactly. A misspelled/renamed field
// is an ERROR, not a silent no-op; the model once sent {count,ranges} and
// believed it had edited the file.
FS_TEST(edit_file_requires_exact_range_fields) {
  fstest::TempDir td("editstrict");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  {
    std::ofstream ofs((proj / "a.txt").string(), std::ios::binary);
    ofs << "alpha\nbeta\ngamma\ndelta\n";
  }
  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();
  FsResult rr = reg.invoke(ctx, "read_file", Json{{"path", "a.txt"}});
  ASSERT_TRUE(rr.ok());
  std::string hash = rr.data["hash"].get<std::string>();
  // Wrong shape (the exact mistake reported): must fail loudly.
  FsResult bad = reg.invoke(
      ctx, "edit_file",
      Json{{"path", "a.txt"},
           {"hash", hash},
           {"edits", Json::array({Json{{"count", 1},
                                       {"ranges", Json::array({Json{{"start", 2},
                                                                    {"end", 2},
                                                                    {"newBytes", 4}}})}}})}});
  EXPECT_FALSE(bad.ok());
  EXPECT_TRUE(bad.message.find("unknown field") != std::string::npos ||
              bad.message.find("start") != std::string::npos);
  // start without end must fail too.
  FsResult half = reg.invoke(
      ctx, "edit_file",
      Json{{"path", "a.txt"},
           {"hash", hash},
           {"edits", Json::array({Json{{"start", 2}, {"newText", "B"}}})}});
  EXPECT_FALSE(half.ok());
  // Correct range succeeds and returns both hashes.
  FsResult ok = reg.invoke(
      ctx, "edit_file",
      Json{{"path", "a.txt"},
           {"hash", hash},
           {"edits", Json::array({Json{{"start", 2}, {"end", 2}, {"newText", "BETA"}}})}});
  ASSERT_TRUE(ok.ok());
  EXPECT_TRUE(ok.data.contains("hash"));
  EXPECT_TRUE(ok.data.contains("beforeHash"));
  // read_file shows real line numbers by default; numbers=false is raw.
  FsResult numbered = reg.invoke(ctx, "read_file", Json{{"path", "a.txt"}});
  ASSERT_TRUE(numbered.ok());
  EXPECT_TRUE(numbered.data["content"].get<std::string>().find(
                  "2\xE2\x94\x82" "BETA") != std::string::npos);
  FsResult raw = reg.invoke(ctx, "read_file",
                            Json{{"path", "a.txt"}, {"numbers", false}});
  ASSERT_TRUE(raw.ok());
  EXPECT_EQ(raw.data["content"].get<std::string>(), std::string("alpha\nBETA\ngamma\ndelta\n"));
}


// Tab-indented files must not make the line-number gutter look like content:
// the separator is U+2502, and the bytes after it are the untouched line.
FS_TEST(read_file_gutter_is_not_a_tab) {
  fstest::TempDir td("readgutter");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  {
    std::ofstream ofs((proj / "tabs.txt").string(), std::ios::binary);
    ofs << "\talpha\n\t\tbeta\n";
  }
  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();
  FsResult r = reg.invoke(ctx, "read_file", Json{{"path", "tabs.txt"}});
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data["content"].get<std::string>(),
            std::string("1\xE2\x94\x82\talpha\n2\xE2\x94\x82\t\tbeta\n"));
  EXPECT_TRUE(r.data["lineFormat"].get<std::string>().find("N\xE2\x94\x82text") !=
              std::string::npos);
}

// ---------- ask：手动模式发事件；自动模式拒绝 ----------
FS_TEST(ask_tool_modes) {
  ToolRegistry reg;
  register_ask_tool(&reg);
  // 手动模式：工具只声明"在等用户"，不自己回答、也不自己发事件——
  // 发 ask.prompt 与挂起是 LOOP 的职责（工具返回 awaiting_user 后 loop park）。
  // 之前工具直接发事件并返回 asked:true，模型以为用户已经回答了。
  ToolContext ctx;
  ctx.auto_mode = false;
  Json args = Json::object();
  args["question"] = "which?";
  args["options"] = Json::array({"a", "b"});
  FsResult r = reg.invoke(ctx, "ask", args);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(jsonutil::get<bool>(r.data, "awaiting_user", false));
  EXPECT_EQ(jsonutil::get<std::string>(r.data, "question", ""), std::string("which?"));
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

// ---------- 编码：读文件必须永远产出合法 UTF-8 ----------
// The turn used to DIE here: read_file returned raw bytes, the GBK text went into
// a tool result, json::dump() threw on the invalid UTF-8 and the exception
// unwound the loop with nothing persisted. These cases pin the decoder and the
// never-throwing serializer.
FS_TEST(utf8_decodes_legacy_encodings) {
  // GBK bytes of 这是一段中文测试文本用于编码检测 (a Chinese Windows .txt).
  const char gbk_raw[] =
      "\xD5\xE2\xCA\xC7\xD2\xBB\xB6\xCE\xD6\xD0\xCE\xC4\xB2\xE2\xCA\xD4\xCE\xC4"
      "\xB1\xBE\xD3\xC3\xD3\xDA\xB1\xE0\xC2\xEB\xBC\xEC\xB2\xE2";
  std::string enc;
  std::string out = utf8::decode_to_utf8(std::string(gbk_raw), &enc);
  EXPECT_TRUE(utf8::valid(out));
  EXPECT_TRUE(!out.empty());
#ifdef _WIN32
  // On Windows the system codepage (or the GB18030 heuristic for 16 CJK chars)
  // decides; either way the Chinese must be readable.
  EXPECT_TRUE(enc == "gbk" || enc == "gb18030");
  EXPECT_TRUE(out.find("\xE8\xBF\x99") != std::string::npos);  // 这
#endif

  // UTF-8 BOM is stripped, not kept as content.
  enc.clear();
  EXPECT_EQ(utf8::decode_to_utf8(std::string("\xEF\xBB\xBFhello", 8), &enc),
            std::string("hello"));
  EXPECT_EQ(enc, std::string("utf-8-bom"));

  // UTF-16LE with BOM (Notepad's "Unicode" save).
  std::string u16("\xFF\xFE", 2);
  const char* hi = "hi";
  for (const char* p = hi; *p; ++p) {
    u16.push_back(*p);
    u16.push_back('\0');
  }
  enc.clear();
  EXPECT_EQ(utf8::decode_to_utf8(u16, &enc), std::string("hi"));
  EXPECT_EQ(enc, std::string("utf-16le"));

  // Arbitrary bytes still produce VALID UTF-8 (the Latin-1 fallback): the point
  // is that nothing downstream can throw.
  const char junk[] = "\x81\xFE\xFF\x80 abc";
  std::string jout = utf8::decode_to_utf8(std::string(junk, 8), nullptr);
  EXPECT_TRUE(utf8::valid(jout));

  // A 5-byte "ab\0cd" is NOT UTF-16: the old heuristic decoded it as CJK and the
  // binary guard stopped recognising it.
  std::string bin("ab\0cd", 5);
  std::string bout = utf8::decode_to_utf8(bin, nullptr);
  EXPECT_TRUE(bout.find('\0') != std::string::npos);

  // dump_safe never throws on invalid UTF-8 and reports U+FFFD instead.
  Json j = Json::object();
  j["bad"] = std::string("\xD5\xE2", 2);
  std::string dumped = dump_safe(j);
  EXPECT_TRUE(utf8::valid(dumped));
}

// ---------- 文件工具：read/ls/find/write/edit/rm（含二进制守卫+逃逸）----------
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
  FsResult rr = reg.invoke(ctx, "read_file", rargs);
  ASSERT_TRUE(rr.ok());
  EXPECT_EQ(rr.data["content"].get<std::string>(), std::string("2\xE2\x94\x82line2\n"));
  // Three lines, not four: the trailing newline terminates line 3 rather than
  // starting an empty line 4 (an edit addressed at line 4 would otherwise
  // silently append instead of failing).
  EXPECT_EQ((int)rr.data["totalLines"].get<int64_t>(), 3);
  // The hash identifies the whole file, so an edit can prove it read this one.
  ASSERT_TRUE(rr.data.contains("hash"));
  std::string file_hash = rr.data["hash"].get<std::string>();
  EXPECT_TRUE(file_hash.size() >= 8);

  // 逃逸拒绝
  Json eargs = Json::object();
  eargs["path"] = "../outside.txt";
  EXPECT_FALSE(reg.invoke(ctx, "read_file", eargs).ok());

  // 二进制守卫
  {
    std::ofstream ofs((proj / "bin.dat").string(), std::ios::binary);
    ofs << std::string("ab\0cd", 5);
  }
  Json bargs = Json::object();
  bargs["path"] = "bin.dat";
  EXPECT_FALSE(reg.invoke(ctx, "read_file", bargs).ok());

  // A GBK file is decoded, not passed through raw. This is the read that used to
  // kill a whole turn (invalid UTF-8 -> json::dump throws -> nothing persisted).
  {
    const char gbk_raw[] =
        "\xD5\xE2\xCA\xC7\xD2\xBB\xB6\xCE\xD6\xD0\xCE\xC4\xB2\xE2\xCA\xD4\xCE"
        "\xC4\xB1\xBE\xD3\xC3\xD3\xDA\xB1\xE0\xC2\xEB\xBC\xEC\xB2\xE2";
    std::ofstream ofs((proj / "gbk.txt").string(), std::ios::binary);
    ofs << std::string(gbk_raw);
  }
  Json encargs = Json::object();
  encargs["path"] = "gbk.txt";
  FsResult encr = reg.invoke(ctx, "read_file", encargs);
  ASSERT_TRUE(encr.ok());
  std::string gcontent = jsonutil::get<std::string>(encr.data, "content", "");
  EXPECT_TRUE(utf8::valid(gcontent));
  EXPECT_TRUE(gcontent.find("\xE8\xBF\x99") != std::string::npos);  // 这
#ifdef _WIN32
  EXPECT_TRUE(encr.data.contains("encoding"));
#endif

  // ls
  Json largs = Json::object();
  largs["path"] = ".";
  FsResult lr = reg.invoke(ctx, "list_dir", largs);
  ASSERT_TRUE(lr.ok());
  EXPECT_TRUE(lr.data["entries"].is_array());
  EXPECT_TRUE((int)lr.data["entries"].size() >= 2);

  // find 文件名
  Json fargs = Json::object();
  fargs["pattern"] = "a.txt";
  FsResult fr = reg.invoke(ctx, "find_files", fargs);
  ASSERT_TRUE(fr.ok());
  EXPECT_TRUE(fr.data["results"].size() >= 1);

  // find grep
  Json gargs = Json::object();
  gargs["pattern"] = "line2";
  gargs["grep"] = true;
  FsResult gr = reg.invoke(ctx, "find_files", gargs);
  ASSERT_TRUE(gr.ok());
  EXPECT_TRUE(gr.data["results"].size() >= 1);

  // A recursive grep does NOT walk generated trees. Reading and decoding every
  // file under build/ is what froze the app on a real project ("temp 几百个文件
  // 扫描极其卡顿"); the skip is reported, not silent.
  {
    ghc::filesystem::create_directories(proj / "build");
    std::ofstream ofs((proj / "build" / "gen.txt").string(), std::ios::binary);
    ofs << "needle-in-build\n";
  }
  Json skargs = Json::object();
  skargs["pattern"] = "needle-in-build";
  skargs["grep"] = true;
  FsResult sk = reg.invoke(ctx, "find_files", skargs);
  ASSERT_TRUE(sk.ok());
  EXPECT_EQ((int)sk.data["results"].size(), 0);
  EXPECT_TRUE(sk.data.contains("note"));

  // edit_file refuses a file it would have to load whole: the unbounded read
  // was the freeze (a stray path to a build artifact moved the machine into
  // swap), and "not found" would be a lie about a file that is right there.
  {
    std::ofstream ofs((proj / "huge.bin").string(), std::ios::binary);
    std::string chunk(1 << 20, 'x');
    for (int i = 0; i < 33; ++i) ofs.write(chunk.data(), chunk.size());
  }
  Json hargs = Json::object();
  hargs["path"] = "huge.bin";
  // The size guard fires before the hash is checked, but the schema still
  // requires a hash; any non-empty value gets us to the guard.
  hargs["hash"] = "0000000000000000";
  hargs["edits"] = Json::array({Json{{"start", 1}, {"end", 1}, {"newText", "x"}}});
  FsResult hr = reg.invoke(ctx, "edit_file", hargs);
  EXPECT_FALSE(hr.ok());
  EXPECT_TRUE(hr.message.find("too large") != std::string::npos);

  // write
  Json wargs = Json::object();
  wargs["path"] = "new.txt";
  wargs["content"] = "hello";
  FsResult wr = reg.invoke(ctx, "write_file", wargs);
  ASSERT_TRUE(wr.ok());
  EXPECT_EQ(wr.data["after"].get<std::string>(), std::string("hello"));

  // edit：行号 + hash
  Json e1 = Json::object();
  e1["path"] = "a.txt";
  e1["hash"] = file_hash;
  e1["edits"] = Json::array({Json{{"start", 2}, {"end", 2}, {"newText", "LINE2"}}});
  FsResult er = reg.invoke(ctx, "edit_file", e1);
  ASSERT_TRUE(er.ok());
  EXPECT_EQ((int)er.data["changes"].size(), 1);
  EXPECT_EQ(er.data["changes"][0]["before"].get<std::string>(), std::string("line2"));
  // A successful edit returns the new hash, so the next edit does not have to
  // re-read the file.
  ASSERT_TRUE(er.data.contains("hash"));
  std::string hash2 = er.data["hash"].get<std::string>();

  // 哈希过期 -> 拒绝（文件被外部改过）
  {
    std::ofstream ofs((proj / "a.txt").string(), std::ios::binary | std::ios::app);
    ofs << "appended\n";
  }
  Json e2 = Json::object();
  e2["path"] = "a.txt";
  e2["hash"] = hash2;
  e2["edits"] = Json::array({Json{{"start", 1}, {"end", 1}, {"newText", "X"}}});
  FsResult er2 = reg.invoke(ctx, "edit_file", e2);
  EXPECT_FALSE(er2.ok());
  EXPECT_TRUE(er2.data.contains("stale"));

  // 重新读取后再改：区间替换只校验 hash
  FsResult rr2 = reg.invoke(ctx, "read_file", Json{{"path", "a.txt"}});
  ASSERT_TRUE(rr2.ok());
  Json e3 = Json::object();
  e3["path"] = "a.txt";
  e3["hash"] = rr2.data["hash"];
  e3["edits"] = Json::array({Json{{"start", 1}, {"end", 1}, {"newText", "X"}}});
  FsResult er3 = reg.invoke(ctx, "edit_file", e3);
  ASSERT_TRUE(er3.ok());
  EXPECT_EQ(er3.data["changes"][0]["before"].get<std::string>(), std::string("line1"));
  FsResult rr3a = reg.invoke(ctx, "read_file", Json{{"path", "a.txt"}});
  ASSERT_TRUE(rr3a.ok());
  EXPECT_TRUE(rr3a.data["content"].get<std::string>().find("1\xE2\x94\x82X") == 0);

  // 行号越界 -> 拒绝（不会静默追加）
  Json e4 = Json::object();
  e4["path"] = "a.txt";
  e4["hash"] = rr3a.data["hash"];
  e4["edits"] = Json::array({Json{{"start", 999}, {"end", 999}, {"newText", "X"}}});
  EXPECT_FALSE(reg.invoke(ctx, "edit_file", e4).ok());

  // 重叠编辑 -> 拒绝（顺序歧义）
  Json e5 = Json::object();
  e5["path"] = "a.txt";
  e5["edits"] = Json::array({Json{{"start", 1}, {"end", 2}, {"newText", "A"}},
                             Json{{"start", 2}, {"end", 3}, {"newText", "B"}}});
  EXPECT_FALSE(reg.invoke(ctx, "edit_file", e5).ok());

  // 删除行（newText 空）。Line 3 is still "line3": line 2 became "LINE2" and a
  // line was appended at the end, so the numbering is what the read reports.
  FsResult rr3 = reg.invoke(ctx, "read_file", Json{{"path", "a.txt"}});
  ASSERT_TRUE(rr3.ok());
  Json e6 = Json::object();
  e6["path"] = "a.txt";
  e6["hash"] = rr3.data["hash"];
  e6["edits"] = Json::array({Json{{"start", 3}, {"end", 3}, {"newText", ""}}});
  FsResult er6 = reg.invoke(ctx, "edit_file", e6);
  ASSERT_TRUE(er6.ok());
  FsResult rr4 = reg.invoke(ctx, "read_file", Json{{"path", "a.txt"}});
  ASSERT_TRUE(rr4.ok());
  EXPECT_TRUE(rr4.data["content"].get<std::string>().find("line3") == std::string::npos);

  // rm
  Json rmargs = Json::object();
  rmargs["path"] = "new.txt";
  FsResult rmr = reg.invoke(ctx, "remove_file", rmargs);
  ASSERT_TRUE(rmr.ok());
  EXPECT_FALSE(reg.invoke(ctx, "remove_file", rmargs).ok());  // 删过再删 -> 不存在
}

#include "agent/agent_host.hpp"
#include "agent/history_builder.hpp"
#include "core/session_manager.hpp"
#include "core/project_manager.hpp"
#include "core/settings_store.hpp"
#include "provider/provider_client.hpp"
#include "provider/provider_runtime.hpp"
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

// ---------- M3 手动端到端：send(模型回文本+write_file)->tool 落库 ----------
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

  // AgentHost：FakeNet 模型（回文本 + write_file 工具调用）
  FakeNet3 net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"done",
    "tool_calls":[{"id":"c1","type":"function",
    "function":{"name":"write_file","arguments":"{\"path\":\"out.txt\",\"content\":\"hi\"}"}}]}}],
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

  // 工具执行接线：write_file 走真实文件工具（ToolRegistry 直接调）
  // AgentLoop 的工具执行由 Host 驱动——v1 验证 send 走通 + 事件
  // approve 档：手动模式下每次写文件都先问（permissive 档项目内直接放行）
  FsResult sendr = host.send(sid, "write out.txt", Json::object(), false, "approve");
  EXPECT_TRUE(sendr.ok());
  // 模型回了 write_file（高风险）-> waiting + permission.prompt
  bool has_prompt = false;
  for (auto& e : events) {
    if (e == "permission.prompt") has_prompt = true;
  }
  EXPECT_TRUE(has_prompt);

  // 批准后继续（Loop 内部流转；v1 验证状态机不停滞）
  // 直接调 tool_invoke 执行 write_file（cwd=项目根）
  // 注意：Host 的 make_ctx cwd 为空——v1 用 ToolRegistry 直调验证工具本身
  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext tctx;
  tctx.cwd = proj_dir.string();
  Json wargs = Json::object();
  wargs["path"] = "out.txt";
  wargs["content"] = "hi";
  FsResult wr = reg.invoke(tctx, "write_file", wargs);
  ASSERT_TRUE(wr.ok());
  // 落库 turn
  Json turn = Json::object();
  turn["turnId"] = "t1";
  turn["role"] = "assistant";
  turn["streamSummary"] = "done";
  turn["toolCalls"] = Json::array({Json{{"name", "write_file"},
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

  // 读回 full.json：user turn（发送时就落库）+ 手动追加的 t1 = 2 轮。
  // 用户输入在 turn 开始时就写盘，正是为了让"关掉标签页再回来"能看到自己说过什么。
  Json full;
  EXPECT_TRUE(sm.load_full(sid, &full).ok());
  EXPECT_EQ((int)full["turns"].size(), 2);

  // 回滚到 t1 -> t1 及其之后被裁掉；t1 之前的 user turn 保留。
  std::vector<std::string> rerrs;
  EXPECT_TRUE(sm.rollback(sid, "t1", &rerrs).ok());
  Json full2;
  EXPECT_TRUE(sm.load_full(sid, &full2).ok());
  EXPECT_EQ((int)full2["turns"].size(), 1);
}

// ---------- HistoryBuilder：全量回放 + 遗忘剔除 + 水位压缩 ----------
FS_TEST(history_builder_flow) {
  HistoryBuilder::Options opts;
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

  // 压缩现在只由水位（或显式"立即压缩"）触发——token 预算不再是触发器。
  opts.force_compress = true;
  opts.keep_turns = 1;
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
  call["name"] = "read_file";
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
    if (b.is_object() && b["type"] == "tool_use" && b["name"] == "read_file") {
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
  EXPECT_FALSE(reg.invoke(ctx, "summarize_session", bad).ok());
  Json good = Json::object();
  good["points"] = Json::array({"fixed login bug", "added retry"});
  FsResult r = reg.invoke(ctx, "summarize_session", good);
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(emitted);
  EXPECT_EQ((int)r.data["recorded"].get<int64_t>(), 2);

  // 压缩附加要点。"立即压缩"是显式触发（token 预算不再是触发器）。
  HistoryBuilder::Options opts;
  opts.force_compress = true;
  opts.keep_turns = 1;
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
  // The budget is a hard ceiling, not the primary trigger: compaction is driven
  // by the context water level (history.compressAtPct). 32000 keeps a large
  // window usable instead of compacting at ~8k tokens.
  EXPECT_EQ(s.get(kKeyHistoryTokenBudget).value.get<int64_t>(), 32000);
  // The threshold triggers a FULL compression. There is no separate strategy
  // setting; short view is only the too-small-window fallback.
  EXPECT_EQ(s.get(kKeyHistoryCompressAtPct).value.get<int64_t>(), 80);
  // Deprecated: compaction folds every turn, so no raw suffix is kept.
  EXPECT_EQ(s.get(kKeyHistoryKeepTurns).value.get<int64_t>(), 0);
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
      call["name"] = "write_file";
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

// ---------- 流式 usage：缓存命中必须从真实字段解析出来 ----------
// The streamed path used to read only "prompt_tokens_cached" (a key no provider
// sends), so the cache-hit rate stayed empty. DeepSeek spells it
// prompt_cache_hit_tokens; OpenAI nests it under prompt_tokens_details.
FS_TEST(provider_stream_usage_reports_cache_hit) {
  ProviderConn conn;
  conn.id = "p1";
  conn.base_url = "http://127.0.0.1:1/v1";
  conn.api_key = "k";
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array();
  req.stream = true;

  {
    FakeNet3 net;
    HttpResponse resp;
    resp.status = 200;
    resp.body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,"
        "\"completion_tokens\":12,\"prompt_cache_hit_tokens\":800}}\n\n"
        "data: [DONE]\n\n";
    net.responses.push_back(resp);
    ProviderClient client(conn, &net);
    ChatResponse out;
    FsResult r = client.chat(req, &out);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(out.text, std::string("hi"));
    EXPECT_EQ(out.in_tokens, int64_t(1000));
    EXPECT_EQ(out.cache_tokens, int64_t(800));
    EXPECT_TRUE(out.cache_known);
  }
  {
    FakeNet3 net;
    HttpResponse resp;
    resp.status = 200;
    resp.body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"yo\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":500,"
        "\"completion_tokens\":3,\"prompt_tokens_details\":{\"cached_tokens\":200}}}\n\n"
        "data: [DONE]\n\n";
    net.responses.push_back(resp);
    ProviderClient client(conn, &net);
    ChatResponse out;
    FsResult r = client.chat(req, &out);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(out.cache_tokens, int64_t(200));
    EXPECT_TRUE(out.cache_known);
    // 200 / 500 = 40% - the ratio the panel shows.
    EXPECT_EQ(out.cache_tokens * 100 / out.in_tokens, int64_t(40));
  }
}

// ---------- 重试：无输出才重试；已经有半截回答就不重试 ----------
namespace {
class RetryNet : public NetClient {
 public:
  mutable int calls = 0;
  // 0 = fail twice then succeed; 1 = always fail after emitting a piece.
  int mode = 0;
  FsResult request(const HttpRequest&, HttpResponse* out) const override {
    out->status = 500;
    return FsResult::error(ErrCode::kRemote, "no plain requests expected");
  }
  FsResult request_stream(
      const HttpRequest&, const std::function<bool(const char*, size_t)>& on_chunk,
      HttpResponse* out) const override {
    ++calls;
    if (mode == 1) {
      // A REAL partial answer: a valid SSE delta, then the connection drops.
      std::string half = "data: {\"choices\":[{\"delta\":{\"content\":\"half\"}}]}\n\n";
      if (on_chunk) on_chunk(half.data(), half.size());
      return FsResult::error(ErrCode::kNetwork, "relay dropped mid-stream");
    }
    if (calls < 3) return FsResult::error(ErrCode::kNetwork, "relay dropped");
    out->status = 200;
    std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
                       "data: [DONE]\n\n";
    if (on_chunk) on_chunk(body.data(), body.size());
    return FsResult::success();
  }
};
}  // namespace

FS_TEST(provider_retries_a_dropped_relay) {
  ProviderConn conn;
  conn.id = "p2";
  conn.base_url = "http://127.0.0.1:1/v1";
  conn.api_key = "k";
  RetryNet net;
  ProviderRuntime rt(conn, &net, nullptr, nullptr);
  rt.retry_policy().max_retries = 3;
  rt.retry_policy().backoff_ms = 0;  // no sleeping in the test
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array();
  req.stream = true;
  std::string seen;
  ChatResponse out;
  FsResult r = rt.call(req, &out, [&seen](const std::string& d) { seen += d; });
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(net.calls, 3);
  EXPECT_EQ(out.text, std::string("ok"));
  EXPECT_EQ(seen, std::string("ok"));
}

FS_TEST(provider_does_not_retry_after_partial_output) {
  ProviderConn conn;
  conn.id = "p3";
  conn.base_url = "http://127.0.0.1:1/v1";
  conn.api_key = "k";
  RetryNet net;
  net.mode = 1;
  ProviderRuntime rt(conn, &net, nullptr, nullptr);
  rt.retry_policy().max_retries = 3;
  rt.retry_policy().backoff_ms = 0;
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array();
  req.stream = true;
  std::string seen;
  ChatResponse out;
  FsResult r = rt.call(req, &out, [&seen](const std::string& d) { seen += d; });
  // A retry would replay "half" from the start and duplicate it on screen.
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(net.calls, 1);
  EXPECT_EQ(seen, std::string("half"));
}
