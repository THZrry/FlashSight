// test_ui_v3.cpp - multi-question ask, cost estimation, markdown blocks
//
// Reports from the same session as test_ui_flow, one round later:
//   * ask showed one question at a time with options rendered as text;
//     the model can ask several things in one reply.
//   * DeepSeek reports usage but no money, so the cost panel showed nothing -
//     the estimate path (unit prices x tokens) did not exist.
//   * markdown had no blockquote, table, rule or link support.
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "agent/agent_loop.hpp"
#include "common/fs_json.hpp"
#include "provider/provider_client.hpp"
#include "provider/provider_types.hpp"

using namespace fs;

namespace {

struct Rec {
  std::vector<std::string> events;
  std::vector<Json> prompts;
  EventFn fn() {
    return [this](const std::string& e, const Json& p) {
      events.push_back(e);
      if (e == "ask.prompt") prompts.push_back(p);
    };
  }
  bool saw(const std::string& e) const {
    for (auto& x : events) {
      if (x == e) return true;
    }
    return false;
  }
};

ModelCallResult tool_reply(std::initializer_list<Json> calls) {
  ModelCallResult r;
  for (auto& c : calls) r.tool_calls.push_back(c);
  return r;
}
Json call(const std::string& id, const std::string& name, Json args) {
  return Json{{"id", id}, {"name", name}, {"args", args}};
}

class FakeNetCost : public NetClient {
 public:
  mutable std::vector<HttpResponse> responses;
  FsResult request(const HttpRequest&, HttpResponse* out) const override {
    if (responses.empty()) {
      out->status = 500;
      return FsResult::error(ErrCode::kRemote, "no canned");
    }
    *out = responses.front();
    responses.erase(responses.begin());
    return FsResult::success();
  }
};

}  // namespace

// ---------- 一次回复里的多个提问合并成一个 ask.prompt ----------
FS_TEST(ask_collects_all_questions_in_one_reply) {
  AgentLoop::Options opts;
  opts.sid = "s_multi_ask";
  opts.permission = "full";
  Rec rec;
  opts.event = rec.fn();
  AgentLoop loop(std::move(opts));
  int turn = 0;
  std::vector<Json> requests;
  loop.call_model = [&](const ModelCallRequest& req, ModelCallResult* out,
                        const std::function<void(const std::string&)>&) {
    requests.push_back(req.messages);
    ++turn;
    if (turn == 1) {
      *out = tool_reply({
          call("a1", "ask", Json{{"question", "which db?"},
                                 {"options", Json::array({"sqlite", "postgres"})},
                                 {"recommended", 1}}),
          call("a2", "ask", Json{{"question", "which port?"},
                                 {"options", Json::array({"8080", "9090"})},
                                 {"recommended", 0}}),
      });
    } else {
      *out = ModelCallResult{"done", Json::array(), "", 0, 0, 0};
    }
    return FsResult::success();
  };
  loop.invoke_tool = [](const ToolContext&, const std::string&, const Json&) {
    return FsResult::success(Json{{"ok", true}});
  };

  ASSERT_TRUE(loop.send("set it up").ok());
  EXPECT_TRUE(loop.state() == LoopState::kWaiting);
  // ONE prompt carrying both questions: the UI renders "1/2".
  ASSERT_TRUE(rec.prompts.size() == 1);
  const Json& p = rec.prompts[0];
  ASSERT_TRUE(p.contains("asks"));
  ASSERT_TRUE(p["asks"].is_array());
  EXPECT_EQ(static_cast<int>(p["asks"].size()), 2);
  EXPECT_EQ(jsonutil::get<std::string>(p["asks"][0], "question", ""),
            std::string("which db?"));
  EXPECT_EQ(jsonutil::get<std::string>(p["asks"][1], "question", ""),
            std::string("which port?"));
  int id1 = static_cast<int>(jsonutil::get_int(p["asks"][0], "askId", 0));
  int id2 = static_cast<int>(jsonutil::get_int(p["asks"][1], "askId", 0));
  EXPECT_TRUE(id1 > 0 && id2 > 0 && id1 != id2);

  // Answering only one keeps the turn parked.
  ASSERT_TRUE(loop.answer_ask(id1, "postgres").ok());
  EXPECT_TRUE(loop.state() == LoopState::kWaiting);

  // Answering the second resumes, and BOTH results are in the transcript.
  ASSERT_TRUE(loop.answer_ask(id2, "9090").ok());
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
  ASSERT_TRUE(requests.size() >= 2);
  int tool_msgs = 0;
  bool saw_db = false, saw_port = false;
  for (auto& m : requests[1]) {
    if (jsonutil::get<std::string>(m, "role", "") != "tool") continue;
    ++tool_msgs;
    std::string c = jsonutil::get<std::string>(m, "content", "");
    if (c.find("postgres") != std::string::npos) saw_db = true;
    if (c.find("9090") != std::string::npos) saw_port = true;
  }
  EXPECT_EQ(tool_msgs, 2);
  EXPECT_TRUE(saw_db);
  EXPECT_TRUE(saw_port);
}

// ===========================================================================
// cost: reported vs estimated
// ===========================================================================

// ---------- 供应商回报金额 -> money ----------
FS_TEST(cost_reported_money) {
  FakeNetCost net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],
    "usage":{"prompt_tokens":1000,"completion_tokens":500},"cost":"0.00123"})";
  net.responses.push_back(ok);
  ProviderConn conn;
  conn.id = "p";
  conn.kind = ApiKind::kCustom;
  conn.base_url = "https://api.test/v1";
  conn.api_key = "k";
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array({Json{{"role", "user"}, {"content", "hi"}}});
  ChatResponse out;
  ASSERT_TRUE(client.chat(req, &out, nullptr).ok());
  EXPECT_EQ(out.cost_kind, std::string("money"));
  EXPECT_TRUE(out.cost > 0.001 && out.cost < 0.002);
}

// ---------- 供应商不回报金额 -> unknown（绝不按写死价格估算）----------
// A number computed from list prices looks exactly like a bill, and the user
// cannot tell it apart from what the provider actually charged. So a provider
// that reports no money shows "unknown" - the token counts are still there for
// anyone who wants to do the arithmetic themselves.
FS_TEST(cost_unknown_when_endpoint_reports_nothing) {
  FakeNetCost net;
  HttpResponse ok;
  ok.status = 200;
  // DeepSeek shape: usage with cache fields, no cost field at all.
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],
    "usage":{"prompt_tokens":1000000,"completion_tokens":1000000,
             "prompt_cache_hit_tokens":500000}})";
  net.responses.push_back(ok);
  ProviderConn conn;
  conn.id = "p";
  conn.kind = ApiKind::kCustom;
  conn.base_url = "https://api.test/v1";
  conn.api_key = "k";
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array({Json{{"role", "user"}, {"content", "hi"}}});
  ChatResponse out;
  ASSERT_TRUE(client.chat(req, &out, nullptr).ok());
  // The cache field must still be picked up: the cache-hit rate is a real
  // measurement even when the price is unknown.
  EXPECT_EQ(static_cast<int>(out.cache_tokens), 500000);
  EXPECT_EQ(out.cost_kind, std::string("unknown"));
  EXPECT_TRUE(out.cost == 0.0);
}

// ---------- 没有单价 -> unknown（不编数字）----------
FS_TEST(cost_unknown_without_prices) {
  FakeNetCost net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],
    "usage":{"prompt_tokens":100,"completion_tokens":50}})";
  net.responses.push_back(ok);
  ProviderConn conn;
  conn.id = "p";
  conn.kind = ApiKind::kCustom;
  conn.base_url = "https://api.test/v1";
  conn.api_key = "k";
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array({Json{{"role", "user"}, {"content", "hi"}}});
  ChatResponse out;
  ASSERT_TRUE(client.chat(req, &out, nullptr).ok());
  EXPECT_EQ(out.cost_kind, std::string("unknown"));
  EXPECT_TRUE(out.cost == 0.0);
}
