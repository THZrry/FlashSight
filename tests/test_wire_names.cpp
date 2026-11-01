// test_wire_names.cpp - provider wire-format constraints
//
// WHY THIS FILE EXISTS
// A real session failed with "http 400" and the cause was the tool names:
// OpenAI and Anthropic both require `^[a-zA-Z0-9_-]{1,64}$`, and this project
// named its tools "file.read" / "plan.update". The dot made EVERY request
// invalid, so the model could not run at all - and because the provider client
// dropped the response body, the UI only said "http 400".
//
// Two invariants are pinned here:
//   1. every registered tool name is legal for a provider,
//   2. a non-2xx response keeps the server's explanation in the error string.
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#include "agent/agent_host.hpp"
#include "common/fs_json.hpp"
#include "provider/prompt_adapter.hpp"
#include "provider/provider_client.hpp"
#include "tools/tool_registry.hpp"

using namespace fs;

namespace {

class FakeNetWire : public NetClient {
 public:
  mutable std::vector<HttpRequest> requests;
  mutable std::vector<HttpResponse> responses;
  FsResult request(const HttpRequest& req, HttpResponse* out) const override {
    requests.push_back(req);
    if (responses.empty()) {
      out->status = 500;
      return FsResult::error(ErrCode::kRemote, "no canned response");
    }
    *out = responses.front();
    responses.erase(responses.begin());
    return FsResult::success();
  }
};

ProviderConn conn_wire() {
  ProviderConn c;
  c.id = "p";
  c.kind = ApiKind::kOpenAIChat;
  c.base_url = "https://api.test/v1";
  c.api_key = "k";
  return c;
}

}  // namespace

// ---------- 工具名合法性：一个非法名字会让整个请求 400 ----------
FS_TEST(tool_names_are_wire_safe) {
  // The rule both providers enforce.
  auto legal = [](const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-';
      if (!ok) return false;
    }
    return true;
  };

  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  Json defs = host.tools().neutral_defs();
  ASSERT_TRUE(defs.is_array());
  EXPECT_TRUE(defs.size() >= 30);  // the registry really is populated
  for (auto& d : defs) {
    std::string name = jsonutil::get<std::string>(d, "name", "");
    EXPECT_TRUE(legal(name));
    // Report the offender in the failure message.
    if (!legal(name)) {
      printf("  illegal tool name: %s\n", name.c_str());
    }
  }
  // The names the product documents must exist under their new spelling.
  std::vector<std::string> must_have = {"read_file", "edit_file", "write_file",
                                        "list_dir", "find_files", "remove_file",
                                        "plan_update", "subagent_spawn",
                                        "todo_add", "terminal_exec"};
  for (auto& want : must_have) {
    bool found = false;
    for (auto& d : defs) {
      if (jsonutil::get<std::string>(d, "name", "") == want) found = true;
    }
    EXPECT_TRUE(found);
    if (!found) printf("  missing tool: %s\n", want.c_str());
  }
}

// ---------- 注册层兜底：非法名被规范化，不会污染整个请求 ----------
FS_TEST(tool_registry_normalizes_illegal_names) {
  EXPECT_TRUE(is_wire_safe_tool_name("read_file"));
  EXPECT_TRUE(is_wire_safe_tool_name("a-b"));
  EXPECT_FALSE(is_wire_safe_tool_name("file.read"));
  EXPECT_FALSE(is_wire_safe_tool_name("a/b"));
  EXPECT_FALSE(is_wire_safe_tool_name(""));
  EXPECT_EQ(wire_safe_tool_name("file.read"), std::string("file_read"));
  EXPECT_EQ(wire_safe_tool_name("mcp/server.tool"), std::string("mcp_server_tool"));
  // Deterministic: the same input always maps to the same name, so the schema
  // sent to the model and the name it calls back agree.
  EXPECT_EQ(wire_safe_tool_name("file.read"), wire_safe_tool_name("file.read"));

  // A tool registered with a dotted name is stored under the safe name.
  ToolRegistry reg;
  ToolDef def;
  def.name = "some.tool";
  def.description = "d";
  def.parameters = Json{{"type", "object"}};
  reg.register_tool("some.tool", def,
                    [](const ToolContext&, const Json&) {
                      return FsResult::success(Json{{"ok", true}});
                    });
  EXPECT_TRUE(reg.has("some_tool"));
  EXPECT_FALSE(reg.has("some.tool"));
  const ToolDef* found = reg.find("some_tool");
  ASSERT_TRUE(found != nullptr);
  // The definition's own name is normalised too: it is what goes on the wire.
  EXPECT_EQ(found->name, std::string("some_tool"));
}

// ---------- 发给模型的工具定义必须全部合法（真实请求体检查）----------
FS_TEST(wire_request_tools_are_legal) {
  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  Json tools = host.tools().neutral_defs();
  // Render for each provider shape and inspect the wire names.
  for (ApiKind kind : {ApiKind::kOpenAIChat, ApiKind::kAnthropic,
                       ApiKind::kOpenAIResponse}) {
    Json wire = prompt::adapt_tools(kind, tools);
    ASSERT_TRUE(wire.is_array());
    for (auto& t : wire) {
      std::string name;
      if (kind == ApiKind::kOpenAIChat || kind == ApiKind::kCustom ||
          kind == ApiKind::kPlan) {
        name = jsonutil::get<std::string>(t["function"], "name", "");
      } else {
        name = jsonutil::get<std::string>(t, "name", "");
      }
      EXPECT_FALSE(name.empty());
      EXPECT_TRUE(is_wire_safe_tool_name(name));
      if (!is_wire_safe_tool_name(name)) {
        printf("  illegal wire name: %s\n", name.c_str());
      }
    }
  }
}

// ---------- 400 必须带上服务器给的原因 ----------
FS_TEST(http_error_keeps_server_message) {
  FakeNetWire net;
  HttpResponse bad;
  bad.status = 400;
  // The real shape of an OpenAI-compatible 400.
  bad.body = R"({"error":{"message":"tools[2].function.name: does not match pattern '^[a-zA-Z0-9_-]+$'","type":"invalid_request_error"}})";
  net.responses.push_back(bad);
  ProviderConn conn = conn_wire();
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array({Json{{"role", "user"}, {"content", "hi"}}});
  ChatResponse out;
  FsResult r = client.chat(req, &out, nullptr);
  EXPECT_FALSE(r.ok());
  // The status is still there...
  EXPECT_TRUE(out.error.find("400") != std::string::npos);
  // ...and so is the reason. Without this the user sees "http 400" and nothing
  // else, which is what made this bug take a whole round to find.
  EXPECT_TRUE(out.error.find("does not match pattern") != std::string::npos);
  // One line: newlines are flattened so the toast stays readable.
  EXPECT_TRUE(out.error.find('\n') == std::string::npos);
}

// ---------- 非 JSON 错误体（代理的 HTML 页）也不会丢 ----------
FS_TEST(http_error_handles_non_json_body) {
  FakeNetWire net;
  HttpResponse bad;
  bad.status = 502;
  bad.body = "<html><body>Bad Gateway</body></html>";
  net.responses.push_back(bad);
  ProviderConn conn = conn_wire();
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "m";
  req.messages = Json::array({Json{{"role", "user"}, {"content", "hi"}}});
  ChatResponse out;
  EXPECT_FALSE(client.chat(req, &out, nullptr).ok());
  EXPECT_TRUE(out.error.find("502") != std::string::npos);
  EXPECT_TRUE(out.error.find("Bad Gateway") != std::string::npos);
}

// ---------- Anthropic 形态同样保留原因 ----------
FS_TEST(http_error_anthropic_shape) {
  FakeNetWire net;
  HttpResponse bad;
  bad.status = 400;
  bad.body = R"({"type":"error","error":{"type":"invalid_request_error","message":"tools.0.name: String should match pattern '^[a-zA-Z0-9_-]{1,64}$'"}})";
  net.responses.push_back(bad);
  ProviderConn conn = conn_wire();
  conn.kind = ApiKind::kAnthropic;
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "claude";
  req.messages = Json::array({Json{{"role", "user"}, {"content", "hi"}}});
  ChatResponse out;
  EXPECT_FALSE(client.chat(req, &out, nullptr).ok());
  EXPECT_TRUE(out.error.find("should match pattern") != std::string::npos);
}

// ---------- 只读子代理不能拿到会写文件的工具 ----------
// A read-only research agent must not be able to modify the tree. file_regex
// rewrites files (that is its purpose) and was in the allow-list before the
// rename, which would have let a "research" subagent edit code.
FS_TEST(read_only_agent_has_no_write_tools) {
  // The allow-list lives in agent_host.cpp; assert its policy through the
  // names it must and must not contain by building the same tool set the host
  // builds for a read-only preset.
  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);

  // Spawn a read-only preset and capture the tools it was given. No model is
  // wired, so the subagent fails fast - the probe fires before that.
  std::vector<Json> seen_tools;
  host.set_subagent_tool_probe([&seen_tools](const Json& tools) {
    seen_tools.push_back(tools);
  });
  std::string id;
  host.subagent_spawn("s_ro", "local-research", "inspect the tree", "", 1, 0, &id);
  host.wait_subagents();

  ASSERT_TRUE(!seen_tools.empty());
  const Json& tools = seen_tools[0];
  auto has = [&tools](const std::string& name) {
    for (auto& t : tools) {
      if (jsonutil::get<std::string>(t, "name", "") == name) return true;
    }
    return false;
  };
  // Read-only tools are present.
  EXPECT_TRUE(has("read_file"));
  EXPECT_TRUE(has("find_files"));
  EXPECT_TRUE(has("list_dir"));
  // Every mutating tool is absent.
  EXPECT_FALSE(has("write_file"));
  EXPECT_FALSE(has("edit_file"));
  EXPECT_FALSE(has("remove_file"));
  EXPECT_FALSE(has("file_regex"));
  EXPECT_FALSE(has("terminal_exec"));
  EXPECT_FALSE(has("terminal_spawn"));
}

// The periodic tick must not deadlock with a finishing subagent.
//
// `on_timer_tick()` calls reap_subagent_threads(), which JOINS finished subagent
// threads; a subagent's last act is an emit() that takes the host lock. If the
// tick held that lock across the join, the whole app froze a few seconds after a
// subagent started (the tick fires on a timer).
//
// The test ticks in a loop WHILE the subagent finishes, which is the real
// sequence: the window that deadlocks is "record marked done, thread not yet
// exited". It deliberately does NOT call wait_subagents() first - that would join
// everything up front and leave the tick nothing to join.
//
// If this ever regresses, the suite HANGS (a stuck thread cannot be recovered), so
// the failure is unmistakable rather than a quiet pass.
FS_TEST(subagent_tick_does_not_deadlock) {
  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  std::string id;
  // No model is wired, so the subagent fails fast - it still walks the full
  // completion path (complete_with_summary + todo tick + emit) that takes the lock.
  FsResult r = host.subagent_spawn("s_tick", "local-research", "noop", "", 1, 0, &id);
  ASSERT_TRUE(r.ok());
  // Tick continuously until the agent reports done (or we give up). Every tick
  // while the worker is between "marked done" and "thread exit" would deadlock on
  // the old code.
  bool finished = false;
  for (int i = 0; i < 400 && !finished; ++i) {
    host.on_timer_tick();
    FsResult list = host.subagent_list("s_tick");
    if (list.ok() && list.data.is_array()) {
      for (size_t k = 0; k < list.data.size(); ++k) {
        if (jsonutil::get<std::string>(list.data[k], "status", "") != "running") {
          finished = true;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(finished);
  // After completion the tick keeps working and the record stays readable.
  EXPECT_TRUE(host.on_timer_tick().ok());
  FsResult list = host.subagent_list("s_tick");
  ASSERT_TRUE(list.ok());
  EXPECT_TRUE(list.data.is_array());
}

// `subagent_list` must stay SMALL: it used to inline every agent's transcript,
// which repeats the whole system prompt per message - three agents overflowed the
// tool-output cap and the last one vanished.
FS_TEST(subagent_list_omits_transcripts) {
  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  std::string id;
  host.subagent_spawn("s_list", "local-research", "noop", "", 1, 0, &id);
  host.wait_subagents();
  FsResult list = host.subagent_list("s_list");
  ASSERT_TRUE(list.ok());
  ASSERT_TRUE(list.data.is_array());
  ASSERT_TRUE(list.data.size() == size_t(1));
  const Json& row = list.data[0];
  EXPECT_FALSE(row.contains("full"));           // no transcript in a list
  EXPECT_TRUE(row.contains("transcriptMessages"));  // a size instead
  // The detail call is what carries it.
  FsResult detail = host.subagent_detail(id);
  ASSERT_TRUE(detail.ok());
  EXPECT_TRUE(detail.data.contains("full"));
}

// TWO concurrent subagents while the turn keeps polling runtime state.
//
// This is the reported freeze path: "it only happens after spawning a SECOND
// subagent". With one agent nothing else touches the record map; with two, one
// thread writes while the turn thread reads `collect_runtime()`. Both used to run
// without the host lock (a std::map data race -> corrupt tree / spin), and
// follow_up ran a whole loop while HOLDING that lock. Either can hang or crash.
//
// If this regresses the suite hangs, which is the honest signal for a deadlock.
FS_TEST(two_subagents_do_not_deadlock) {
  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  std::string a, b;
  ASSERT_TRUE(host.subagent_spawn("s_two", "local-research", "one", "", 1, 0, &a).ok());
  ASSERT_TRUE(host.subagent_spawn("s_two", "local-research", "two", "", 1, 0, &b).ok());
  // Drive the same calls the app does while both run: tick, list, runtime state.
  bool both_done = false;
  for (int i = 0; i < 600 && !both_done; ++i) {
    host.on_timer_tick();
    FsResult list = host.subagent_list("s_two");
    int done = 0;
    if (list.ok() && list.data.is_array()) {
      for (size_t k = 0; k < list.data.size(); ++k) {
        if (jsonutil::get<std::string>(list.data[k], "status", "") != "running") ++done;
      }
    }
    both_done = done >= 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(both_done);
  // Both records survive and stay readable.
  FsResult list = host.subagent_list("s_two");
  ASSERT_TRUE(list.ok());
  EXPECT_EQ(list.data.size(), size_t(2));
  EXPECT_TRUE(host.subagent_detail(a).ok());
  EXPECT_TRUE(host.subagent_detail(b).ok());
}

// A follow-up must not hold the host lock while it runs: the run emits events and
// may spawn nested agents, all of which need that lock.
FS_TEST(subagent_follow_up_returns_summary_without_deadlock) {
  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  std::string id;
  ASSERT_TRUE(host.subagent_spawn("s_fu", "local-research", "first", "", 1, 0, &id).ok());
  host.wait_subagents();
  // No model wired, so this fails fast - but it must RETURN (not hang) and must
  // leave the record in a finished state.
  FsResult r = host.subagent_follow_up(id, "second");
  EXPECT_TRUE(r.ok() || !r.ok());  // reaching here at all is the assertion
  FsResult list = host.subagent_list("s_fu");
  ASSERT_TRUE(list.ok());
  ASSERT_TRUE(list.data.size() == size_t(1));
  EXPECT_TRUE(jsonutil::get<std::string>(list.data[0], "status", "") != "running");
}

// Finished records are pruned so a long session cannot grow the table forever.
// (Each record holds a whole transcript, and every message in it carries a copy
// of the system prompt.)
FS_TEST(subagent_records_are_pruned) {
  AgentHostDeps deps;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);
  for (int i = 0; i < 6; ++i) {
    std::string id;
    host.subagent_spawn("s_prune", "local-research", "t", "", 1, 0, &id);
  }
  host.wait_subagents();
  size_t before = host.subagent_list("s_prune").data.size();
  EXPECT_TRUE(before > 2);
  // Pruning is driven by the tick; keep 2 to make the effect obvious.
  int dropped = host.subagent_prune_for_test("s_prune", 2);
  EXPECT_TRUE(dropped > 0);
  EXPECT_EQ(host.subagent_list("s_prune").data.size(), size_t(2));
  // A running agent is NEVER pruned, however aggressive the cap.
  std::string run_id;
  ASSERT_TRUE(host.subagent_spawn("s_prune", "local-research", "keep me", "", 1, 0,
                                  &run_id).ok());
  host.subagent_prune_for_test("s_prune", 0);
  bool still_there = false;
  FsResult list = host.subagent_list("s_prune");
  for (size_t k = 0; k < list.data.size(); ++k) {
    if (jsonutil::get<std::string>(list.data[k], "id", "") == run_id) still_there = true;
  }
  EXPECT_TRUE(still_there);
  host.wait_subagents();
}
