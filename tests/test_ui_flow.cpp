// test_ui_flow.cpp - ask parking, auto-round markers, and turn persistence
//
// Three reports from a real session:
//   1. "ask 在界面不会 suspend 任务，而是静默失败" - the ask tool returned
//      asked:true and the turn carried on; the choice never reached the model.
//   2. "自动轮没有任何预告 / 主页没有紫色小圆点 / 自动轮和历史混在一起" -
//      auto rounds were invisible and their stored nudge rendered as a user
//      message.
//   3. "我原本输入告诉我失败了，结果后台跑起来了，回来看有输出文档输入却显示
//      不出来了" - a failed turn persisted the answer but not the question.
#include <memory>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "agent/agent_context.hpp"
#include "agent/agent_host.hpp"
#include "agent/agent_loop.hpp"
#include "common/fs_json.hpp"

using namespace fs;

namespace {

struct Rec {
  std::vector<std::string> events;
  std::vector<Json> payloads;
  EventFn fn() {
    return [this](const std::string& e, const Json& p) {
      events.push_back(e);
      if (e == "ask.prompt" || e == "auto.next" || e == "loop.paused") {
        payloads.push_back(p);
      }
    };
  }
  bool saw(const std::string& e) const {
    for (auto& x : events) {
      if (x == e) return true;
    }
    return false;
  }
};

ModelCallResult text_reply(const std::string& t) {
  ModelCallResult r;
  r.text = t;
  return r;
}
ModelCallResult tool_reply(std::initializer_list<Json> calls) {
  ModelCallResult r;
  for (auto& c : calls) r.tool_calls.push_back(c);
  return r;
}
Json call(const std::string& id, const std::string& name, Json args) {
  return Json{{"id", id}, {"name", name}, {"args", args}};
}

}  // namespace

// ---------- ask 挂起本轮，回答后回填并继续 ----------
FS_TEST(ask_parks_the_turn_until_answered) {
  AgentLoop::Options opts;
  opts.sid = "s_ask";
  opts.permission = "full";
  Rec rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));

  std::vector<Json> requests;
  int turn = 0;
  loop.call_model = [&](const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>&) {
    requests.push_back(req.messages);
    ++turn;
    if (turn == 1) {
      *out = tool_reply({call("a1", "ask",
                              Json{{"question", "which database?"},
                                   {"options", Json::array({"sqlite", "postgres"})},
                                   {"recommended", 0}})});
    } else {
      // Second round: the answer must be in the transcript.
      *out = text_reply("ok, using postgres");
    }
    return FsResult::success();
  };
  bool tool_ran = false;
  loop.invoke_tool = [&tool_ran](const ToolContext&, const std::string&, const Json&) {
    tool_ran = true;
    return FsResult::success(Json{{"ok", true}});
  };

  ASSERT_TRUE(loop.send("set up the db").ok());
  // THE bug: the turn used to keep going. Now it parks.
  EXPECT_TRUE(loop.state() == LoopState::kWaiting);
  EXPECT_FALSE(tool_ran);  // ask is not executed as a normal tool
  EXPECT_TRUE(rec.saw("ask.prompt"));
  ASSERT_TRUE(!rec.payloads.empty());
  int ask_id = static_cast<int>(jsonutil::get_int(rec.payloads[0], "askId", 0));
  EXPECT_TRUE(ask_id > 0);
  EXPECT_EQ(jsonutil::get<std::string>(rec.payloads[0], "question", ""),
            std::string("which database?"));
  ASSERT_TRUE(rec.payloads[0]["options"].is_array());
  EXPECT_EQ(static_cast<int>(rec.payloads[0]["options"].size()), 2);

  // Answer it: the turn resumes and the selection is fed back as the tool result.
  ASSERT_TRUE(loop.answer_ask(ask_id, "postgres").ok());
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
  ASSERT_TRUE(requests.size() >= 2);
  bool answer_in_history = false;
  for (auto& m : requests[1]) {
    if (jsonutil::get<std::string>(m, "role", "") != "tool") continue;
    std::string c = jsonutil::get<std::string>(m, "content", "");
    if (c.find("postgres") != std::string::npos) answer_in_history = true;
  }
  EXPECT_TRUE(answer_in_history);
}

// ---------- 过期的回答不能解决当前问题 ----------
FS_TEST(ask_rejects_stale_answer) {
  AgentLoop::Options opts;
  opts.sid = "s_ask2";
  opts.permission = "full";
  Rec rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    *out = tool_reply({call("a1", "ask",
                            Json{{"question", "q"}, {"options", Json::array({"x"})}})});
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("go").ok());
  ASSERT_TRUE(loop.state() == LoopState::kWaiting);
  int ask_id = static_cast<int>(jsonutil::get_int(rec.payloads[0], "askId", 0));
  EXPECT_FALSE(loop.answer_ask(ask_id + 999, "x").ok());
  EXPECT_TRUE(loop.state() == LoopState::kWaiting);
  EXPECT_TRUE(loop.answer_ask(ask_id, "x").ok());
}

// ---------- 自动轮：先发预告，并带上轮数 ----------
FS_TEST(auto_round_is_announced_with_round_number) {
  AgentLoop::Options opts;
  opts.sid = "s_auto";
  opts.permission = "full";
  opts.max_auto_continues = 3;
  opts.context.auto_mode = true;
  Rec rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  int step = 0;
  loop.runtime_state = [&step]() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.todos = Json::array({Json{{"id", 1}, {"task", "work"}, {"done", step >= 2}}});
    return st;
  };
  loop.call_model = [&step](const ModelCallRequest&, ModelCallResult* out,
                            const std::function<void(const std::string&)>&) {
    ++step;
    *out = text_reply("round " + std::to_string(step));
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("do it").ok());
  EXPECT_TRUE(rec.saw("auto.next"));
  ASSERT_TRUE(!rec.payloads.empty());
  // The round number is what the UI shows; it must be >= 1.
  int round = 0;
  for (auto& p : rec.payloads) {
    if (p.contains("round")) round = static_cast<int>(jsonutil::get_int(p, "round", 0));
  }
  EXPECT_TRUE(round >= 1);
  // And the loop reports it while the round runs, so the UI can paint the dot.
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
}

// ---------- 自动轮的存档带 autoRound 标记（不冒充用户输入）----------
FS_TEST(auto_round_persists_marker) {
  AgentLoop::Options opts;
  opts.sid = "s_auto2";
  opts.permission = "full";
  opts.max_auto_continues = 1;
  opts.context.auto_mode = true;
  AgentLoop loop(std::move(opts));
  std::vector<Json> persisted;
  loop.persist_turn = [&persisted](const Json& t) { persisted.push_back(t); };
  int step = 0;
  loop.runtime_state = [&step]() {
    RuntimeState st;
    st.now_ms = 1700000000000LL;
    st.todos = Json::array({Json{{"id", 1}, {"task", "w"}, {"done", step >= 2}}});
    return st;
  };
  loop.call_model = [&step](const ModelCallRequest&, ModelCallResult* out,
                            const std::function<void(const std::string&)>&) {
    ++step;
    *out = text_reply("r" + std::to_string(step));
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("start").ok());
  // The first user turn is the real one; the auto round's turn carries autoRound.
  bool saw_auto_user_turn = false;
  for (auto& t : persisted) {
    if (jsonutil::get<std::string>(t, "role", "") != "user") continue;
    if (jsonutil::get_int(t, "autoRound", 0) > 0) {
      saw_auto_user_turn = true;
      EXPECT_TRUE(jsonutil::get<bool>(t, "system", false));
    }
  }
  EXPECT_TRUE(saw_auto_user_turn);
}

// ---------- 首发失败：用户输入仍然落库 ----------
FS_TEST(failed_turn_still_persists_user_input) {
  AgentLoop::Options opts;
  opts.sid = "s_fail";
  opts.permission = "full";
  AgentLoop loop(std::move(opts));
  std::vector<Json> persisted;
  loop.persist_turn = [&persisted](const Json& t) { persisted.push_back(t); };
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    out->error = "http 400: bad request";
    return FsResult::error(ErrCode::kRemote, "http 400: bad request");
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json::object());
  };
  ASSERT_TRUE(loop.send("remember this question").ok() == false);
  // The send failed, but the question must be on disk.
  bool found = false;
  for (auto& t : persisted) {
    if (jsonutil::get<std::string>(t, "role", "") == "user" &&
        jsonutil::get<std::string>(t, "promptRaw", "") == "remember this question") {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// ---------- 工具上限是"暂停"而不是"中断" ----------
FS_TEST(tool_limit_pauses_instead_of_failing) {
  AgentLoop::Options opts;
  opts.sid = "s_limit";
  opts.permission = "full";
  opts.max_tool_rounds = 3;
  Rec rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  loop.call_model = [](const ModelCallRequest&, ModelCallResult* out,
                       const std::function<void(const std::string&)>&) {
    // A different call every time, so the repeat guard never fires.
    static int n = 0;
    *out = tool_reply({call("c" + std::to_string(n), "read_file",
                            Json{{"path", "f" + std::to_string(n) + ".txt"}})});
    ++n;
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };
  ASSERT_TRUE(loop.send("go").ok());
  EXPECT_TRUE(rec.saw("loop.paused"));
  // Not an interruption: the UI must not show "failed, continue?" for this.
  EXPECT_FALSE(rec.saw("loop.interrupted"));
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
}
