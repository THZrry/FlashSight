// test_loop_flow.cpp - agent loop: multi-call groups, approval gate, auto mode
//
// These cover the paths that used to be structurally broken:
//   * several tool calls in one reply (only the first was ever answered),
//   * an approval in the middle of a group (the rest were dropped),
//   * tool results being replayed to the model (they were not),
//   * auto mode continuing on an open plan (it never continued).
#include <chrono>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#include "agent/agent_context.hpp"
#include "agent/agent_host.hpp"
#include "agent/agent_loop.hpp"
#include "common/fs_json.hpp"

using namespace fs;

namespace {

struct Recorder {
  std::vector<std::string> events;
  std::vector<Json> prompts;
  std::vector<Json> tool_results;

  EventFn fn() {
    return [this](const std::string& e, const Json& p) {
      events.push_back(e);
      if (e == "permission.prompt") prompts.push_back(p);
      if (e == "tool.update") tool_results.push_back(p);
    };
  }
  bool saw(const std::string& e) const {
    for (auto& x : events) {
      if (x == e) return true;
    }
    return false;
  }
  int count(const std::string& e) const {
    int n = 0;
    for (auto& x : events) {
      if (x == e) ++n;
    }
    return n;
  }
};

// A model that replays a scripted list of replies, then answers with text.
struct ScriptedModel {
  std::vector<ModelCallResult> replies;
  size_t idx = 0;
  std::vector<Json> seen_messages;  // messages of each request, for assertions

  std::function<FsResult(const ModelCallRequest&, ModelCallResult*,
                         const std::function<void(const std::string&)>&)>
  fn() {
    return [this](const ModelCallRequest& req, ModelCallResult* out,
                  const std::function<void(const std::string&)>& on_delta) -> FsResult {
      seen_messages.push_back(req.messages);
      if (idx < replies.size()) {
        *out = replies[idx++];
      } else {
        out->text = "done";
        out->tool_calls = Json::array();
      }
      if (on_delta && !out->text.empty()) on_delta(out->text);
      return FsResult::success();
    };
  }
};

ModelCallResult tool_reply(std::initializer_list<Json> calls) {
  ModelCallResult r;
  r.text = "";
  r.tool_calls = Json::array();
  for (auto& c : calls) r.tool_calls.push_back(c);
  return r;
}

Json call(const std::string& id, const std::string& name, Json args) {
  return Json{{"id", id}, {"name", name}, {"args", args}};
}

}  // namespace

// ---------- 一轮里多个工具调用：每个都要执行并各自回填 ----------
FS_TEST(loop_multi_tool_calls_all_answered) {
  AgentLoop::Options opts;
  opts.sid = "s_multi";
  opts.permission = "full";
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  model.replies.push_back(tool_reply({
      call("c1", "read_file", Json{{"path", "a.txt"}}),
      call("c2", "read_file", Json{{"path", "b.txt"}}),
  }));
  loop.call_model = model.fn();
  std::vector<std::string> invoked;
  loop.invoke_tool = [&invoked](const ToolContext&, const std::string& name,
                                const Json& args) -> FsResult {
    invoked.push_back(name + ":" + jsonutil::get<std::string>(args, "path", ""));
    return FsResult::success(Json{{"ok", true}, {"summary", "ok"}});
  };

  ASSERT_TRUE(loop.send("read both").ok());
  EXPECT_EQ((int)invoked.size(), 2);
  EXPECT_EQ(invoked[0], std::string("read_file:a.txt"));
  EXPECT_EQ(invoked[1], std::string("read_file:b.txt"));
  // The second request must carry BOTH tool results, each with its call id.
  ASSERT_TRUE(model.seen_messages.size() >= 2);
  const Json& second = model.seen_messages[1];
  int tool_msgs = 0;
  std::vector<std::string> ids;
  for (auto& m : second) {
    if (jsonutil::get<std::string>(m, "role", "") != "tool") continue;
    ++tool_msgs;
    ids.push_back(jsonutil::get<std::string>(m, "toolCallId", ""));
  }
  EXPECT_EQ(tool_msgs, 2);
  ASSERT_TRUE(ids.size() == 2);
  EXPECT_EQ(ids[0], std::string("c1"));
  EXPECT_EQ(ids[1], std::string("c2"));
}

// ---------- 工具轮里的"过程说明"要单独发事件并落库 ----------
// The model's narration between tool calls ("let me check the config first") is
// NOT the answer. It used to be invisible: only the last round's text was kept,
// so the narration vanished on reload, and while streaming it was glued onto the
// answer bubble.
FS_TEST(loop_interim_narration_is_emitted_and_stored) {
  AgentLoop::Options opts;
  opts.sid = "s_interim";
  opts.permission = "full";
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  ModelCallResult first;
  first.text = "let me look first";
  first.tool_calls = Json::array({call("c1", "read_file", Json{{"path", "a.txt"}})});
  model.replies.push_back(first);
  loop.call_model = model.fn();
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) -> FsResult {
    return FsResult::success(Json{{"ok", true}, {"summary", "ok"}});
  };
  Json persisted = Json::array();
  loop.persist_turn = [&persisted](const Json& t) { persisted.push_back(t); };

  ASSERT_TRUE(loop.send("go").ok());
  // The narration was announced as its own event...
  EXPECT_TRUE(rec.count("assistant.interim") >= 1);
  bool saw_text = false;
  for (auto& e : rec.events) {
    if (e == "assistant.interim") saw_text = true;
  }
  EXPECT_TRUE(saw_text);
  // ...and it is stored with the assistant turn, so a reload can show it.
  bool stored = false;
  for (size_t i = 0; i < persisted.size(); ++i) {
    const Json& t = persisted[i];
    if (jsonutil::get<std::string>(t, "role", "") != "assistant") continue;
    if (t.contains("interim") && t["interim"].is_array() && !t["interim"].empty()) {
      stored = true;
      EXPECT_TRUE(t["interim"][0].is_string());
      EXPECT_EQ(t["interim"][0].get<std::string>(), std::string("let me look first"));
    }
    // The final answer stays the final answer.
    EXPECT_EQ(jsonutil::get<std::string>(t, "streamSummary", ""), std::string("done"));
  }
  EXPECT_TRUE(stored);
}

// ---------- 审批门：批准后整组继续，后面的调用不丢 ----------
FS_TEST(loop_approval_mid_group_continues) {  AgentLoop::Options opts;
  opts.sid = "s_gate";
  opts.permission = "approve";  // every write asks
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  model.replies.push_back(tool_reply({
      call("c1", "read_file", Json{{"path", "a.txt"}}),   // no approval needed
      call("c2", "write_file", Json{{"path", "b.txt"}, {"content", "x"}}),  // asks
      call("c3", "read_file", Json{{"path", "c.txt"}}),   // must still run
  }));
  loop.call_model = model.fn();
  std::vector<std::string> invoked;
  loop.invoke_tool = [&invoked](const ToolContext&, const std::string& name,
                                const Json& args) -> FsResult {
    invoked.push_back(name + ":" + jsonutil::get<std::string>(args, "path", ""));
    return FsResult::success(Json{{"ok", true}});
  };

  ASSERT_TRUE(loop.send("do three things").ok());
  EXPECT_TRUE(loop.state() == LoopState::kWaiting);
  EXPECT_EQ((int)invoked.size(), 1);  // only the read ran
  EXPECT_TRUE(rec.saw("permission.prompt"));

  // Approve: the write runs, then the REST of the group (c3) must also run.
  ASSERT_TRUE(loop.approve(true).ok());
  EXPECT_EQ((int)invoked.size(), 3);
  EXPECT_EQ(invoked[1], std::string("write_file:b.txt"));
  EXPECT_EQ(invoked[2], std::string("read_file:c.txt"));
  // And the final request carries three tool messages.
  ASSERT_TRUE(model.seen_messages.size() >= 2);
  int tool_msgs = 0;
  for (auto& m : model.seen_messages.back()) {
    if (jsonutil::get<std::string>(m, "role", "") == "tool") ++tool_msgs;
  }
  EXPECT_EQ(tool_msgs, 3);
}

// ---------- 拒绝：如实回填，模型能换道 ----------
FS_TEST(loop_denial_is_reported) {
  AgentLoop::Options opts;
  opts.sid = "s_deny";
  opts.permission = "approve";
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  model.replies.push_back(
      tool_reply({call("c1", "remove_file", Json{{"path", "keep.txt"}})}));
  loop.call_model = model.fn();
  bool ran = false;
  loop.invoke_tool = [&ran](const ToolContext&, const std::string&, const Json&) {
    ran = true;
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("delete it").ok());
  ASSERT_TRUE(loop.state() == LoopState::kWaiting);
  ASSERT_TRUE(loop.approve(false).ok());
  EXPECT_FALSE(ran);
  // The model must SEE the refusal (a silent drop makes it retry forever).
  ASSERT_TRUE(model.seen_messages.size() >= 2);
  bool found = false;
  for (auto& m : model.seen_messages.back()) {
    if (jsonutil::get<std::string>(m, "role", "") != "tool") continue;
    if (jsonutil::get<std::string>(m, "content", "").find("denied") !=
        std::string::npos) {
      found = true;
      EXPECT_TRUE(jsonutil::get<bool>(m, "isError", false));
    }
  }
  EXPECT_TRUE(found);
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
}

// ---------- 参数 JSON 解析失败：不执行工具，回填错误 ----------
FS_TEST(loop_bad_arguments_not_executed) {
  AgentLoop::Options opts;
  opts.sid = "s_badargs";
  opts.permission = "full";
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  // Wire-shaped call with broken JSON arguments.
  Json wire = Json::object();
  wire["id"] = "c1";
  wire["function"] = Json{{"name", "write_file"}, {"arguments", "{oops"}};
  ModelCallResult r;
  r.tool_calls = Json::array({wire});
  model.replies.push_back(r);
  loop.call_model = model.fn();
  bool ran = false;
  loop.invoke_tool = [&ran](const ToolContext&, const std::string&, const Json&) {
    ran = true;
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("write").ok());
  // A malformed write must never become an empty write.
  EXPECT_FALSE(ran);
  ASSERT_TRUE(model.seen_messages.size() >= 2);
  bool told = false;
  for (auto& m : model.seen_messages.back()) {
    if (jsonutil::get<std::string>(m, "role", "") != "tool") continue;
    if (jsonutil::get<std::string>(m, "content", "").find("valid JSON") !=
        std::string::npos) {
      told = true;
    }
  }
  EXPECT_TRUE(told);
}

// ---------- 重复相同调用：停下并说明，不空转到上限 ----------
FS_TEST(loop_repeat_guard_stops) {
  AgentLoop::Options opts;
  opts.sid = "s_repeat";
  opts.permission = "full";
  opts.max_repeat_calls = 2;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  // The model keeps asking for the same read, ignoring the result.
  for (int i = 0; i < 8; ++i) {
    model.replies.push_back(tool_reply({call("same", "read_file",
                                             Json{{"path", "a.txt"}})}));
  }
  loop.call_model = model.fn();
  int invoked = 0;
  loop.invoke_tool = [&invoked](const ToolContext&, const std::string&, const Json&) {
    ++invoked;
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("loop forever").ok());
  // Bounded: the guard trips long before the script is exhausted.
  EXPECT_TRUE(invoked <= 4);
  // A guard trip is a PAUSE (the turn produced output), not an interruption:
  // `loop.paused` keeps the "continue?" bar out of a turn that did its job.
  EXPECT_TRUE(rec.saw("loop.paused"));
  EXPECT_FALSE(rec.saw("loop.interrupted"));
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
}

// ---------- 自动模式：规划未闭合 -> 自动续跑 ----------
FS_TEST(loop_auto_mode_continues_on_open_plan) {
  AgentLoop::Options opts;
  opts.sid = "s_auto";
  opts.permission = "full";
  opts.max_auto_continues = 3;
  opts.context.auto_mode = true;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  int step = 0;
  loop.runtime_state = [&step]() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    // The plan is a DOCUMENT now; auto mode follows its status.
    st.plan = Json{{"exists", true},
                   {"status", step >= 2 ? "done" : "active"},
                   {"content", "# plan\n- a\n- b"}};
    return st;
  };
  ScriptedModel model;
  model.replies.push_back(ModelCallResult{"step a done", Json::array(), "", 0, 0, 0});
  model.replies.push_back(ModelCallResult{"step b done", Json::array(), "", 0, 0, 0});
  loop.call_model = [&model, &step](const ModelCallRequest& req, ModelCallResult* out,
                                    const std::function<void(const std::string&)>&) {
    model.seen_messages.push_back(req.messages);
    ++step;
    if (model.idx < model.replies.size()) *out = model.replies[model.idx++];
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("do both steps").ok());
  // Two model calls: the first turn plus one automatic continuation.
  EXPECT_TRUE(model.seen_messages.size() >= 2);
  EXPECT_TRUE(rec.saw("auto.next"));
  EXPECT_EQ(rec.count("turn.done"), 2);
  // The continuation carries the plan state, not the whole history again.
  const Json& last = model.seen_messages.back();
  std::string content = jsonutil::get<std::string>(last.back(), "content", "");
  EXPECT_TRUE(content.find("<runtime>") != std::string::npos);
  // Auto-sent messages carry the SYSTEM: prefix so the model (and a reader of the
  // stored transcript) can tell them from something the user typed.
  EXPECT_TRUE(content.find("SYSTEM:") != std::string::npos);
  EXPECT_TRUE(content.find("continue") != std::string::npos);
}

// ---------- 自动模式：plan/todo 都 done 时，本轮跑过工具也不续 ----------
// The fallback "auto + tool ran = continue" must not override explicit tracking:
// a final verification command in a finished task used to start another round.
FS_TEST(loop_auto_stops_when_tracking_done_despite_tools) {
  AgentLoop::Options opts;
  opts.sid = "s_auto_done_tools";
  opts.permission = "full";
  opts.context.auto_mode = true;
  opts.max_auto_continues = 5;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.plan = Json{{"exists", true}, {"status", "done"}, {"content", "# plan"}};
    st.todos = Json::array({Json{{"id", 1}, {"task", "a"}, {"done", true}}});
    return st;
  };
  ScriptedModel model;
  model.replies.push_back(tool_reply({call("c1", "read_file", Json{{"path", "a.txt"}})}));
  loop.call_model = model.fn();
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("verify").ok());
  EXPECT_FALSE(rec.saw("auto.next"));
  EXPECT_EQ(rec.count("turn.done"), 1);
}

// ---------- 自动模式：规划全闭合 -> 不续跑 ----------
FS_TEST(loop_auto_mode_stops_when_plan_done) {
  AgentLoop::Options opts;
  opts.sid = "s_auto2";
  opts.permission = "full";
  opts.context.auto_mode = true;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.plan = Json{{"exists", true},
                   {"status", "done"},
                   {"content", "# plan\n- everything done"}};
    return st;
  };
  ScriptedModel model;
  loop.call_model = model.fn();
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("go").ok());
  EXPECT_EQ((int)model.seen_messages.size(), 1);
  EXPECT_FALSE(rec.saw("auto.next"));
  EXPECT_EQ(rec.count("turn.done"), 1);
}

// ---------- 自动模式：空转一轮就停（清单/规划都没让它再动） ----------
// The real report: five consecutive "模型输出当前没有新的指令，到此结束" rounds on
// an active plan document. An auto round that executes NO tool has nothing to
// build on; one continuation is enough to prove that, then stop.
FS_TEST(loop_auto_stops_after_empty_round) {
  AgentLoop::Options opts;
  opts.sid = "s_auto_empty";
  opts.permission = "full";
  opts.context.auto_mode = true;
  opts.max_auto_continues = 5;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.plan = Json{{"exists", true}, {"status", "active"}, {"content", "# plan"}};
    return st;
  };
  ScriptedModel model;
  loop.call_model = model.fn();  // pure text, no tool calls, every time
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("go").ok());
  // First turn + exactly one auto round; the second auto round is refused.
  EXPECT_EQ((int)model.seen_messages.size(), 2);
  EXPECT_EQ(rec.count("turn.done"), 2);
  EXPECT_TRUE(rec.saw("auto.halted"));
  EXPECT_EQ(rec.count("auto.next"), 1);
}

// ---------- 自动模式：清单没清空就必须继续（空转保护不能压掉它） ----------
// The guard above once suppressed a TODO-driven continuation after a text-only
// round, which looked like "清单没清空，自动轮却不继续". Open items are an
// explicit instruction to keep going; only plan-document-only continuation is
// stopped by the empty-round guard.
FS_TEST(loop_auto_continues_with_open_todos_after_empty_round) {
  AgentLoop::Options opts;
  opts.sid = "s_auto_todos";
  opts.permission = "full";
  opts.context.auto_mode = true;
  opts.max_auto_continues = 10;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.todos = Json::array({Json{{"id", 1}, {"task", "unfinished"}, {"done", false}}});
    return st;
  };
  ScriptedModel model;
  loop.call_model = model.fn();  // text only, no tools
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("go").ok());
  // First turn + 2 auto rounds. The checklist stays open, so this is the
  // empty-round STREAK guard (not the cap) stopping the wake/render loop.
  EXPECT_EQ((int)model.seen_messages.size(), 3);
  EXPECT_EQ(rec.count("auto.next"), 2);
  // A single empty round continues while todos are open; the second stops it.
  EXPECT_TRUE(rec.saw("auto.halted"));
}

// A tab reopened mid-turn reads live_state(). Its `text` must be the CURRENT
// round only; completed-round narration belongs in `interim`, otherwise the
// reopened tab showed thinking as the answer ("切页面思考跑到任务输出").
FS_TEST(loop_live_snapshot_keeps_narration_out_of_the_answer) {
  AgentLoop::Options opts;
  opts.sid = "s_live_split";
  opts.permission = "full";
  AgentLoop loop(std::move(opts));
  std::vector<Json> live_snaps;
  int calls = 0;
  loop.call_model = [&loop, &live_snaps, &calls](
                        const ModelCallRequest&, ModelCallResult* out,
                        const std::function<void(const std::string&)>& on_delta)
      -> FsResult {
    live_snaps.push_back(loop.live_state());
    if (calls++ == 0) {
      out->text = "thinking first";
      out->tool_calls = Json::array({call("c1", "noop", Json::object())});
    } else {
      out->text = "final answer";
    }
    if (on_delta && !out->text.empty()) on_delta(out->text);
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  loop.persist_turn = [](const Json&) {};
  ASSERT_TRUE(loop.send("go").ok());
  ASSERT_TRUE(live_snaps.size() >= 2);
  const Json& after_first = live_snaps[1];
  // Narration is in the work log...
  ASSERT_TRUE(after_first.contains("interim"));
  ASSERT_TRUE(after_first["interim"].is_array());
  ASSERT_TRUE(after_first["interim"].size() >= 1);
  EXPECT_EQ(after_first["interim"][0].get<std::string>(),
            std::string("thinking first"));
  // ...and NOT in the answer text.
  EXPECT_EQ(jsonutil::get<std::string>(after_first, "text", ""), std::string());
}

// ---------- 宿主接线：请求里必须真的有工具表和非空系统提示 ----------
// This is the end-to-end shape check for the wiring bug that made the agent
// useless: AgentLoop was created with no tools and no system prompt, so the
// model received `tools: []` and an empty instruction block.
FS_TEST(host_send_wires_tools_and_system_prompt) {
  Json full = Json::object();
  full["meta"] = Json{{"sid", "s_host"}, {"pid", "p1"}, {"cwd", "C:/proj"},
                      {"title", "t"}};
  full["sessionSettings"] = Json{{"model", "model-x"}, {"filePerm", "full"}};
  full["turns"] = Json::array();

  std::vector<ModelCallRequest> requests;
  AgentHostDeps deps;
  deps.load_full = [&full](const std::string&, Json* out) -> FsResult {
    *out = full;
    return FsResult::success();
  };
  deps.append_turn = [&full](const std::string&, const Json& turn) -> FsResult {
    full["turns"].push_back(turn);
    return FsResult::success();
  };
  deps.call_model = [&requests](const ModelCallRequest& req, ModelCallResult* out,
                                const std::function<void(const std::string&)>&) {
    requests.push_back(req);
    out->text = "ok";
    return FsResult::success();
  };
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);

  ASSERT_TRUE(host.send("s_host", "hello", Json::object(), false, "full", "model-x").ok());
  ASSERT_TRUE(!requests.empty());
  const ModelCallRequest& req = requests[0];
  // Tools: the real registry, not an empty array.
  EXPECT_TRUE(req.tools.is_array());
  EXPECT_TRUE(req.tools.size() > 10);
  bool has_file_read = false, has_plan = false, has_spawn = false;
  for (auto& t : req.tools) {
    std::string n = jsonutil::get<std::string>(t, "name", "");
    if (n == "read_file") has_file_read = true;
    if (n == "plan_update") has_plan = true;
    if (n == "subagent_spawn") has_spawn = true;
  }
  EXPECT_TRUE(has_file_read);
  EXPECT_TRUE(has_plan);
  EXPECT_TRUE(has_spawn);
  // System prompt: built by AgentContext, non-empty, mentions the environment
  // and the planning advice.
  ASSERT_TRUE(req.messages.is_array());
  ASSERT_TRUE(!req.messages.empty());
  EXPECT_EQ(jsonutil::get<std::string>(req.messages[0], "role", ""),
            std::string("system"));
  std::string sys = jsonutil::get<std::string>(req.messages[0], "content", "");
  EXPECT_TRUE(sys.size() > 500);
  EXPECT_TRUE(sys.find("C:/proj") != std::string::npos);
  EXPECT_TRUE(sys.find("Planning") != std::string::npos ||
              sys.find("规划") != std::string::npos);
  // The model from the session settings reached the request.
  EXPECT_EQ(req.model, std::string("model-x"));
  // The newest user message carries the runtime block.
  std::string user = jsonutil::get<std::string>(req.messages.back(), "content", "");
  EXPECT_TRUE(user.find("<runtime>") != std::string::npos);
  EXPECT_TRUE(user.find("hello") != std::string::npos);
}

// A fired wake must actually START the next turn. wake.fired used to be emitted
// into the void: the scheduler fired, the UI showed nothing, and the task never
// resumed ("没能计划唤醒").
FS_TEST(host_wake_fires_a_new_turn) {
  Json full = Json::object();
  full["meta"] = Json{{"sid", "s_wake"}, {"pid", "p1"}, {"cwd", "C:/proj"},
                      {"title", "t"}};
  full["sessionSettings"] = Json{{"model", "model-x"}, {"filePerm", "full"}};
  full["turns"] = Json::array();
  std::vector<ModelCallRequest> requests;
  AgentHostDeps deps;
  deps.load_full = [&full](const std::string&, Json* out) -> FsResult {
    *out = full;
    return FsResult::success();
  };
  deps.append_turn = [&full](const std::string&, const Json& turn) -> FsResult {
    full["turns"].push_back(turn);
    return FsResult::success();
  };
  deps.call_model = [&requests](const ModelCallRequest& req, ModelCallResult* out,
                                const std::function<void(const std::string&)>&) {
    requests.push_back(req);
    out->text = "ok";
    return FsResult::success();
  };
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  ASSERT_TRUE(host.send("s_wake", "start", Json::object(), false, "full",
                        "model-x").ok());
  size_t before = requests.size();
  host.wakes().schedule("s_wake", WakeSource::kTimer, 0, "check the build");
  host.wakes().due(1000);
  // The continuation runs on its own thread (it must not block the scheduler).
  for (int i = 0; i < 300; ++i) {
    if (requests.size() > before && !host.is_running("s_wake")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ((int)requests.size(), (int)before + 1);
  // The SYSTEM wake note is the new user message, not an empty "continue".
  std::string user = jsonutil::get<std::string>(requests.back().messages.back(),
                                                "content", "");
  EXPECT_TRUE(user.find("SYSTEM") != std::string::npos);
  EXPECT_TRUE(user.find("check the build") != std::string::npos);
}

// ---------- 闪退也要有记录：流式期间定期写 live 快照 ----------
// A turn is only appended to the session when it FINISHES, so a crashed process
// (the webview crash) lost the whole answer. The in-memory live snapshot did not
// help - a crash takes memory with it. This writes it to disk while streaming.
FS_TEST(loop_persists_live_snapshot_while_streaming) {
  AgentLoop::Options opts;
  opts.sid = "s_live";
  opts.permission = "full";
  AgentLoop loop(std::move(opts));
  std::vector<Json> snaps;
  loop.persist_live = [&snaps](const std::string& sid, const Json& live) {
    EXPECT_EQ(sid, std::string("s_live"));
    snaps.push_back(live);
  };
  loop.persist_turn = [](const Json&) {};
  // A model that streams a piece and then answers.
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out,
                       const std::function<void(const std::string&)>& on_delta) {
    if (on_delta) on_delta("streaming text");
    out->text = "final answer";
    return FsResult::success();
  };
  ASSERT_TRUE(loop.send("my question").ok());
  // At least two writes: one with the in-flight text (recording the question), and
  // the final one that CLEARS it because the turn landed on disk.
  ASSERT_TRUE(snaps.size() >= 2);
  bool saw_text = false;
  bool saw_user = false;
  for (size_t i = 0; i < snaps.size(); ++i) {
    if (jsonutil::get<std::string>(snaps[i], "text", "") == "streaming text") {
      saw_text = true;
    }
    if (jsonutil::get<std::string>(snaps[i], "userText", "") == "my question") {
      saw_user = true;
    }
  }
  EXPECT_TRUE(saw_text);   // the answer-so-far survives a crash
  EXPECT_TRUE(saw_user);   // ...and so does the question
  // The LAST write clears the snapshot: the turn is on disk, so replaying it as
  // "interrupted" on the next open would duplicate it.
  EXPECT_FALSE(jsonutil::get<bool>(snaps.back(), "running", true));
}

// ---------- 自动续跑的历史里必须有模型上一条回答 ----------
FS_TEST(loop_auto_continue_keeps_last_answer) {
  AgentLoop::Options opts;
  opts.sid = "s_auto3";
  opts.permission = "full";
  opts.context.auto_mode = true;
  opts.max_auto_continues = 1;
  AgentLoop loop(std::move(opts));
  int calls = 0;
  loop.runtime_state = [&calls]() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.plan = Json{{"exists", true},
                   {"status", calls >= 2 ? "done" : "active"},
                   {"content", "# plan\n- a"}};
    return st;
  };
  std::vector<Json> seen;
  loop.call_model = [&](const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>&) {
    ++calls;
    seen.push_back(req.messages);
    out->text = calls == 1 ? "first answer" : "second answer";
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("start").ok());
  ASSERT_TRUE(seen.size() == 2);
  // The continuation request must contain the previous assistant answer before
  // the new user nudge.
  bool has_answer = false;
  for (auto& m : seen[1]) {
    if (jsonutil::get<std::string>(m, "role", "") == "assistant" &&
        jsonutil::get<std::string>(m, "content", "") == "first answer") {
      has_answer = true;
    }
  }
  EXPECT_TRUE(has_answer);
}

// ---------- 压缩结果持久化：下一次 run 直接复用摘要，不重放全文 ----------
FS_TEST(loop_reuses_durable_compaction_state) {
  Json full = Json::object();
  Json turns = Json::array();
  for (int i = 0; i < 4; ++i) {
    Json t = Json::object();
    t["role"] = i % 2 == 0 ? "user" : "assistant";
    t["turnId"] = "t" + std::to_string(i);
    if (i % 2 == 0) t["promptRaw"] = "old question " + std::to_string(i);
    else t["streamSummary"] = "old answer " + std::to_string(i);
    turns.push_back(t);
  }
  full["turns"] = turns;
  Json saved;
  AgentLoop::Options first;
  first.sid = "s_comp";
  first.permission = "full";
  first.history.force_compress = true;
  AgentLoop loop1(std::move(first));
  loop1.load_history = [&full](Json* out) {
    *out = full;
    return FsResult::success();
  };
  loop1.compaction_save = [&saved](const std::string&, const Json& s) { saved = s; };
  loop1.call_model = [](const ModelCallRequest&, ModelCallResult* out,
                        const std::function<void(const std::string&)>&) {
    out->text = "first";
    return FsResult::success();
  };
  ASSERT_TRUE(loop1.send("go").ok());
  ASSERT_TRUE(saved.is_object());
  EXPECT_TRUE(saved.contains("summary"));

  // A NEW loop (next turn / next process) loads that state. The request must be
  // system + durable summary + the new user turn, not the four old turns.
  AgentLoop::Options second;
  second.sid = "s_comp";
  second.permission = "full";
  AgentLoop loop2(std::move(second));
  loop2.compaction_load = [&saved](const std::string&) { return saved; };
  loop2.load_history = [&full](Json* out) {
    *out = full;
    return FsResult::success();
  };
  Json seen;
  loop2.call_model = [&seen](const ModelCallRequest& req, ModelCallResult* out,
                             const std::function<void(const std::string&)>&) {
    seen = req.messages;
    out->text = "second";
    return FsResult::success();
  };
  ASSERT_TRUE(loop2.send("next").ok());
  ASSERT_TRUE(seen.is_array());
  EXPECT_EQ((int)seen.size(), 3);  // system + summary + the new user turn
  std::string all;
  for (auto& m : seen) all += jsonutil::get<std::string>(m, "content", "");
  EXPECT_TRUE(all.find("first") != std::string::npos);
  EXPECT_TRUE(all.find("next") != std::string::npos);
}

// A first short view (no prior full summary) must be persisted too: otherwise
// an existing long session regenerates the same lossy short view on every send.
FS_TEST(loop_persists_first_short_view) {
  Json turns = Json::array();
  for (int i = 0; i < 20; ++i) {
    Json t = Json::object();
    t["role"] = i % 2 == 0 ? "user" : "assistant";
    t["turnId"] = "t" + std::to_string(i);
    t["promptRaw"] = std::string(2000, 'x');
    t["streamSummary"] = std::string(2000, 'x');
    turns.push_back(t);
  }
  Json full = Json::object();
  full["turns"] = turns;
  Json saved;
  AgentLoop::Options opts;
  opts.sid = "s_short";
  opts.permission = "full";
  AgentLoop loop(std::move(opts));
  loop.context_limit = 100;  // even the compressed summary cannot fit
  loop.set_last_prompt_tokens(100);
  loop.load_history = [&full](Json* out) {
    *out = full;
    return FsResult::success();
  };
  loop.compaction_save = [&saved](const std::string&, const Json& s) { saved = s; };
  loop.call_model = [](const ModelCallRequest& req, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    // The summary request must be bigger than this deliberately tiny window,
    // otherwise a compact summary would legitimately fit and mode would be full.
    std::string sys;
    if (!req.messages.empty()) {
      sys = jsonutil::get<std::string>(req.messages[0], "content", "");
    }
    if (sys.find("compress") != std::string::npos ||
        sys.find("压缩") != std::string::npos) {
      out->text = std::string(4000, 's');
    } else {
      out->text = "ok";
    }
    return FsResult::success();
  };
  ASSERT_TRUE(loop.send("go").ok());
  ASSERT_TRUE(saved.is_object());
  EXPECT_EQ(saved.value("mode", ""), std::string("short-view"));
  EXPECT_TRUE(saved.contains("compactedTurns"));
}

// Steering sent while a turn runs belongs to the CURRENT turn: it is injected
// before the next model call, not stored as a second user conversation.
FS_TEST(loop_steering_is_applied_to_the_next_call) {
  AgentLoop::Options opts;
  opts.sid = "s_steer";
  opts.permission = "full";
  AgentLoop loop(std::move(opts));
  std::vector<Json> seen;
  int calls = 0;
  loop.call_model = [&](const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>&) {
    seen.push_back(req.messages);
    ++calls;
    if (calls == 1) {
      Json c = Json::object();
      c["name"] = "read_file";
      c["id"] = "call_1";
      c["args"] = Json{{"path", "a.txt"}};
      out->tool_calls = Json::array({c});
      loop.append("also check b.txt");
    } else {
      out->text = "done";
    }
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("go").ok());
  ASSERT_TRUE(seen.size() >= 2);
  std::string second;
  for (auto& m : seen[1]) second += jsonutil::get<std::string>(m, "content", "");
  EXPECT_TRUE(second.find("also check b.txt") != std::string::npos);
}

// The model's emergency brake stops further auto rounds after the current turn.
FS_TEST(loop_auto_stop_brake) {
  AgentLoop::Options opts;
  opts.sid = "s_autostop";
  opts.permission = "full";
  opts.context.auto_mode = true;
  opts.max_auto_continues = 5;
  opts.event = [](const std::string&, const Json&) {};
  AgentLoop loop(std::move(opts));
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.plan = Json{{"exists", true}, {"status", "active"}, {"content", "# plan"}};
    return st;
  };
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out,
                             const std::function<void(const std::string&)>&) {
    ++calls;
    out->text = "still working";
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  loop.request_auto_stop();
  ASSERT_TRUE(loop.send("go").ok());
  EXPECT_EQ(calls, 1);  // no auto round after the brake
}

// Auto mode must not END just because the model forgot to create a todo/plan:
// a turn that actually ran tools continues, then the empty-round guard stops
// when the next round only narrates.
FS_TEST(loop_auto_continues_after_tool_work_without_todo) {
  AgentLoop::Options opts;
  opts.sid = "s_auto_notodo";
  opts.permission = "full";
  opts.context.auto_mode = true;
  opts.max_auto_continues = 5;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;  // no plan, no todos
    return st;
  };
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out,
                             const std::function<void(const std::string&)>&) {
    ++calls;
    if (calls == 1) {
      Json c = Json::object();
      c["name"] = "read_file";
      c["id"] = "call_1";
      c["args"] = Json{{"path", "a.txt"}};
      out->tool_calls = Json::array({c});
    } else {
      out->text = "done";
    }
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("go").ok());
  EXPECT_EQ(calls, 3);  // initial + after-tool call + one auto round
  EXPECT_EQ(rec.count("auto.next"), 1);
}

// AUTO OFF must stay off even when a tool-heavy turn hits the per-turn round
// limit. This branch used to start a new round without consulting auto_mode.
FS_TEST(loop_round_limit_does_not_auto_continue_in_manual_mode) {
  AgentLoop::Options opts;
  opts.sid = "s_manual_round_limit";
  opts.permission = "full";
  opts.context.auto_mode = false;
  opts.max_tool_rounds = 1;
  opts.max_auto_continues = 5;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    return st;
  };
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out,
                             const std::function<void(const std::string&)>&) {
    ++calls;
    Json c = Json::object();
    c["name"] = "read_file";
    c["id"] = "call_1";
    c["args"] = Json{{"path", "a.txt"}};
    out->tool_calls = Json::array({c});
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("go").ok());
  // Two model calls: the first requests a tool, the second trips the round
  // limit. What matters is that the turn ENDS there in manual mode.
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(rec.count("auto.next"), 0);
}

// The water level can cross the threshold MID-turn (a long tool chain). The next
// auto round must start from a compressed request, not go out at 81%+.
FS_TEST(loop_compacts_between_auto_rounds_at_water_level) {
  AgentLoop::Options opts;
  opts.sid = "s_auto_compact";
  opts.permission = "full";
  opts.context.auto_mode = true;
  opts.max_tool_rounds = 50;
  opts.max_auto_continues = 5;
  opts.history.compress_at_pct = 80;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.context_limit = 10000;
  loop.runtime_state = []() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;  // no plan, no todos
    return st;
  };
  Json full = Json::object();
  full["turns"] = Json::array({
      Json{{"role", "user"}, {"promptRaw", "old question"}},
      Json{{"role", "assistant"}, {"streamSummary", "old answer"}},
  });
  loop.load_history = [&full](Json* out) -> FsResult {
    *out = full;
    return FsResult::success();
  };
  std::vector<Json> seen;
  int calls = 0;
  loop.call_model = [&](const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>&) {
    std::string sys;
    if (!req.messages.empty()) {
      sys = jsonutil::get<std::string>(req.messages[0], "content", "");
    }
    if (sys.find("compress") != std::string::npos ||
        sys.find("压缩") != std::string::npos) {
      out->text = "SUMMARY old question";  // the model summary call
      return FsResult::success();
    }
    ++calls;
    seen.push_back(req.messages);
    out->in_tokens = 8500;  // 85% of 10k -> threshold
    if (calls == 1) {
      Json c = Json::object();
      c["name"] = "read_file";
      c["id"] = "call_1";
      c["args"] = Json{{"path", "a.txt"}};
      out->tool_calls = Json::array({c});
    } else {
      out->text = "done";
    }
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("go").ok());
  EXPECT_EQ(calls, 3);  // initial tool call + final call + one auto round
  ASSERT_TRUE(rec.count("context.compressed") >= 1);
  ASSERT_TRUE(seen.size() >= 3);
  std::string round2;
  for (auto& m : seen[2]) round2 += jsonutil::get<std::string>(m, "content", "");
  EXPECT_TRUE(round2.find("compacted") != std::string::npos);
  EXPECT_TRUE(round2.find("old question") != std::string::npos);
  // System prefix survives the rebuild (an empty one made the auto round blind).
  ASSERT_TRUE(!seen[2].empty());
  EXPECT_FALSE(jsonutil::get<std::string>(seen[2][0], "content", "").empty());
}

// A call that crosses the watermark is followed by a compaction decision even
// when the turn simply ENDS (no auto continuation, no next user send).
FS_TEST(loop_compresses_after_a_final_call_at_water_level) {
  AgentLoop::Options opts;
  opts.sid = "s_end_compact";
  opts.permission = "full";
  opts.context.auto_mode = false;
  opts.history.compress_at_pct = 80;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.context_limit = 10000;
  Json full = Json::object();
  full["turns"] = Json::array({
      Json{{"role", "user"}, {"promptRaw", "old requirement"}},
      Json{{"role", "assistant"}, {"streamSummary", "old answer"}},
  });
  loop.load_history = [&full](Json* out) -> FsResult {
    *out = full;
    return FsResult::success();
  };
  Json saved;
  loop.compaction_save = [&saved](const std::string&, const Json& s) { saved = s; };
  loop.call_model = [](const ModelCallRequest& req, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    std::string sys;
    if (!req.messages.empty()) {
      sys = jsonutil::get<std::string>(req.messages[0], "content", "");
    }
    if (sys.find("compress") != std::string::npos ||
        sys.find("压缩") != std::string::npos) {
      out->text = "SUMMARY old requirement";
      return FsResult::success();
    }
    out->text = "done";
    out->in_tokens = 8500;  // 85%
    return FsResult::success();
  };
  ASSERT_TRUE(loop.send("go").ok());
  EXPECT_TRUE(rec.count("context.compressed") >= 1);
  ASSERT_TRUE(saved.is_object());
  EXPECT_TRUE(saved.contains("summary"));
}

// A model call that produces no token for the progress budget must be aborted,
// not left running for hours (the WhaleJS 01:54→09:00 hang).
FS_TEST(loop_aborts_a_model_call_with_no_progress) {
  AgentLoop::Options opts;
  opts.sid = "s_stall";
  opts.permission = "full";
  opts.model_progress_timeout_ms = 30;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.call_model = [](const ModelCallRequest& req, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    if (req.cancel && req.cancel()) {
      out->error = "model call made no progress";
    } else {
      out->text = "ok";
    }
    return FsResult::success();
  };
  FsResult r = loop.send("go");
  // The turn must end as an interruption, not stay running.
  EXPECT_TRUE(rec.saw("loop.interrupted"));
  EXPECT_TRUE(r.ok());  // the loop reports the provider error through the event
}

// Process shutdown reaches loops that are NOT in AgentHost::loops_ (subagents)
// through Options::external_cancel. Without it, closing the window waited for
// the running subagent forever.
FS_TEST(loop_external_cancel_aborts_a_blocked_call) {
  std::atomic<bool> stopping(false);
  AgentLoop::Options opts;
  opts.sid = "s_shutdown";
  opts.permission = "full";
  opts.model_progress_timeout_ms = 100000;  // only the external flag may fire
  opts.external_cancel = [&stopping]() { return stopping.load(); };
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.call_model = [](const ModelCallRequest& req, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    for (int i = 0; i < 400; ++i) {
      if (req.cancel && req.cancel()) {
        out->error = "shutdown";
        return FsResult::success();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    out->text = "should not finish";
    return FsResult::success();
  };
  FsResult r;
  std::thread t([&]() { r = loop.send("go"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  stopping.store(true);
  t.join();
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(rec.saw("loop.interrupted"));
}

// ---------- 无人值守（子代理）：需要批准的动作直接拒绝，不挂死 ----------
FS_TEST(loop_unattended_denies_instead_of_waiting) {
  AgentLoop::Options opts;
  opts.sid = "s_headless";
  opts.permission = "approve";
  opts.unattended = true;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  model.replies.push_back(
      tool_reply({call("c1", "write_file", Json{{"path", "a.txt"}, {"content", "x"}})}));
  loop.call_model = model.fn();
  bool ran = false;
  loop.invoke_tool = [&ran](const ToolContext&, const std::string&, const Json&) {
    ran = true;
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("write").ok());
  // Never parks waiting: there is nobody to press approve.
  EXPECT_FALSE(loop.state() == LoopState::kWaiting);
  EXPECT_FALSE(ran);
  EXPECT_FALSE(rec.saw("permission.prompt"));
  // The refusal is visible to the model, with a way forward.
  ASSERT_TRUE(model.seen_messages.size() >= 2);
  bool told = false;
  for (auto& m : model.seen_messages.back()) {
    if (jsonutil::get<std::string>(m, "role", "") != "tool") continue;
    std::string c = jsonutil::get<std::string>(m, "content", "");
    if (c.find("unattended") != std::string::npos) told = true;
  }
  EXPECT_TRUE(told);
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
}

// ---------- 防过拟合：开启后重复输出只发提示，不截断 ----------
FS_TEST(loop_overfit_emits_hint_only) {
  // The detector's window is 3, so the model has to answer three times. Each
  // round asks for a DIFFERENT read (the repeat guard must not fire first).
  auto script = [](ScriptedModel* m) {
    for (int i = 0; i < 3; ++i) {
      ModelCallResult r;
      r.text = "the same repeated sentence again";
      r.tool_calls = Json::array(
          {call("c" + std::to_string(i), "read_file",
                Json{{"path", "f" + std::to_string(i) + ".txt"}})});
      m->replies.push_back(r);
    }
    m->replies.push_back(
        ModelCallResult{"the same repeated sentence again", Json::array(), "", 0, 0, 0});
  };

  AgentLoop::Options opts;
  opts.sid = "s_overfit";
  opts.permission = "full";
  opts.overfit_enabled = true;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  script(&model);
  loop.call_model = model.fn();
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("talk").ok());
  EXPECT_TRUE(rec.saw("truncate.hint"));
  // The text itself is NOT cut: the backend only advises the UI.
  EXPECT_EQ(model.replies[0].text, std::string("the same repeated sentence again"));

  // Off by default: no hint.
  AgentLoop::Options off;
  off.sid = "s_overfit2";
  off.permission = "full";
  Recorder rec2;
  off.event = rec2.fn();
  AgentLoop loop2(std::move(off));
  ScriptedModel m2;
  script(&m2);
  loop2.call_model = m2.fn();
  loop2.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop2.send("talk").ok());
  EXPECT_FALSE(rec2.saw("truncate.hint"));
}

FS_TEST(loop_system_prompt_reflects_context) {
  AgentLoop::Options opts;
  opts.sid = "s_sys";
  opts.permission = "full";
  ContextInput in;
  in.lang = ContextLang::kEn;
  in.cwd = "/proj";
  in.model = "model-a";
  in.tools = Json::array({Json{{"name", "read_file"},
                               {"description", "read"},
                               {"parameters", Json{{"type", "object"}}}}});
  opts.context = in;
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  loop.call_model = model.fn();
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("hi").ok());
  ASSERT_TRUE(!model.seen_messages.empty());
  const Json& msgs = model.seen_messages[0];
  ASSERT_TRUE(jsonutil::get<std::string>(msgs[0], "role", "") == "system");
  std::string sys = jsonutil::get<std::string>(msgs[0], "content", "");
  EXPECT_TRUE(sys.find("model-a") != std::string::npos);
  EXPECT_TRUE(sys.find("read_file:") != std::string::npos);
  // The system prompt must be exactly one message, not duplicated per turn.
  int systems = 0;
  for (auto& m : msgs) {
    if (jsonutil::get<std::string>(m, "role", "") == "system") ++systems;
  }
  EXPECT_EQ(systems, 1);
}

// ---------- 抛异常也必须落到终态，不能永远“运行中” ----------
// run_turn sets kPlanning before it persists the user turn. Anything that threw
// there used to escape without touching the loop state, so is_running() stayed
// true forever and a session whose turn failed still showed "running" after a
// tab switch. The wrapper must always end in kTurnDone and announce it.
FS_TEST(loop_exception_in_turn_start_is_terminal) {
  AgentLoop::Options opts;
  opts.sid = "s_throw";
  opts.permission = "full";
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  ScriptedModel model;
  loop.call_model = model.fn();
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  loop.persist_turn = [](const Json&) { throw std::runtime_error("disk full"); };

  FsResult r = loop.send("go");
  EXPECT_FALSE(r.ok());
  // Terminal state: a later session.list / loopState says "not running".
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
  EXPECT_TRUE(rec.saw("loop.interrupted"));
  // The failure is visible in the transcript, not only in a toast.
  EXPECT_FALSE(rec.saw("turn.started"));
}

// /compact runs the summarizer NOW with the user's extra note, not as a next
// model turn.
FS_TEST(loop_compact_now_uses_hint_and_saves) {
  AgentLoop::Options opts;
  opts.sid = "s_compact_now";
  opts.permission = "full";
  ContextInput in;
  in.lang = ContextLang::kEn;
  opts.context = in;
  Recorder rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.context_limit = 100000;
  Json full = Json::object();
  full["turns"] = Json::array({
      Json{{"role", "user"}, {"promptRaw", "old question"}},
      Json{{"role", "assistant"}, {"streamSummary", "old answer"}},
  });
  loop.load_history = [&full](Json* out) -> FsResult {
    *out = full;
    return FsResult::success();
  };
  Json saved = Json::object();
  loop.compaction_save = [&saved](const std::string&, const Json& s) { saved = s; };
  Json summary_req = Json::object();
  loop.call_model = [&summary_req](const ModelCallRequest& req, ModelCallResult* out,
                                   const std::function<void(const std::string&)>&) {
    summary_req = req.messages;
    out->text = "SUMMARY";
    return FsResult::success();
  };
  ASSERT_TRUE(loop.compact_now("keep the public API").ok());
  EXPECT_TRUE(jsonutil::get<std::string>(saved, "summary", "")
                  .find("SUMMARY") != std::string::npos);
  ASSERT_TRUE(summary_req.is_array() && summary_req.size() >= 2);
  std::string user = jsonutil::get<std::string>(summary_req[1], "content", "");
  EXPECT_TRUE(user.find("keep the public API") != std::string::npos);
  EXPECT_TRUE(rec.saw("context.compressed"));
}
