// test_context.cpp - agent context & provider adapter
//
// These tests exist because the context is the product: a wrong prefix costs
// money on every single turn (cache miss), and a wrong tool-result shape makes
// the agent loop forever. Both are invisible in a UI smoke test, so they are
// pinned here.
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "agent/agent_context.hpp"
#include "agent/history_builder.hpp"
#include "common/fs_json.hpp"
#include "provider/prompt_adapter.hpp"

using namespace fs;

namespace {

ContextInput make_ctx(ContextLang lang) {
  ContextInput in;
  in.lang = lang;
  in.cwd = "C:/proj";
  in.os = "windows";
  in.shell = "pwsh";
  in.model = "gpt-4o";
  in.permission = "permissive";
  in.tools = Json::array({
      Json{{"name", "read_file"},
           {"description", "read a file"},
           {"parameters", Json{{"type", "object"}}}},
      Json{{"name", "write_file"},
           {"description", "write a file"},
           {"parameters", Json{{"type", "object"}}}},
      Json{{"name", "todo_add"},
           {"description", "add todo"},
           {"parameters", Json{{"type", "object"}}}},
  });
  return in;
}

}  // namespace

// ---------- 前缀里必须显式给出规划与工作流的使用建议 ----------
// 用户明确要求："一定要在全局信息里面给出对规划和工作流等显式的使用建议"。
FS_TEST(context_planning_and_workflow_guidance_present) {
  ContextInput in = make_ctx(ContextLang::kEn);
  std::string sys = AgentContext::build_system(in);
  // Planning advice: when to use it, how to keep it true. The section carries a
  // stable marker the runtime nudge can refer to ("Remember the [RULES] above").
  EXPECT_TRUE(sys.find("[RULES]") != std::string::npos);
  EXPECT_TRUE(sys.find("todo_add") != std::string::npos);
  EXPECT_TRUE(sys.find("3+ steps") != std::string::npos);
  // Workflow advice: when NOT to use it (the important half).
  EXPECT_TRUE(sys.find("Workflows") != std::string::npos);
  EXPECT_TRUE(sys.find("one-off") != std::string::npos);
  // Subagent advice: the context-cost test, not just "you can spawn".
  EXPECT_TRUE(sys.find("Subagents") != std::string::npos);
  EXPECT_TRUE(sys.find("context cost") != std::string::npos);

  // Chinese variant must carry the same sections (it is the default for zh UI).
  ContextInput zh = make_ctx(ContextLang::kZh);
  std::string zsys = AgentContext::build_system(zh);
  EXPECT_TRUE(zsys.find("规划") != std::string::npos);
  EXPECT_TRUE(zsys.find("工作流") != std::string::npos);
  EXPECT_TRUE(zsys.find("子代理") != std::string::npos);
}

// ---------- 前缀必须稳定（同一输入逐字节相同）----------
FS_TEST(context_prefix_is_deterministic) {
  ContextInput in = make_ctx(ContextLang::kEn);
  std::string a = AgentContext::build_system(in);
  std::string b = AgentContext::build_system(in);
  EXPECT_EQ(a, b);
  // Volatile values must not leak into the prefix: no timestamp, no plan text.
  EXPECT_TRUE(a.find("1970") == std::string::npos);
  EXPECT_TRUE(a.find("<runtime>") == std::string::npos);
}

// ---------- 易变状态放在新用户消息里，且不污染历史 ----------
FS_TEST(context_runtime_block_is_separate) {
  ContextInput in = make_ctx(ContextLang::kEn);
  RuntimeState st;
  st.now_ms = 1700000000000LL;  // 2023-11-14
  st.plan = Json{{"exists", true},
                 {"status", "active"},
                 {"path", ".flashsight/plans/s_x.md"},
                 {"content", "# ship it\n- a\n- b"}};
  st.todos = Json::array({Json{{"id", 1}, {"task", "write test"}, {"done", false}}});
  std::string rt = AgentContext::build_runtime(in, st);
  // The plan is a document: the runtime says it exists / where / its status, and
  // does not dump the body (the model reads it with plan_get).
  EXPECT_TRUE(rt.find("plan document") != std::string::npos);
  EXPECT_TRUE(rt.find("active") != std::string::npos);
  EXPECT_TRUE(rt.find("plans/s_x.md") != std::string::npos);
  EXPECT_TRUE(rt.find("write test") != std::string::npos);
  EXPECT_TRUE(rt.find("2023-11-14") != std::string::npos);
  EXPECT_TRUE(rt.find("1 open") != std::string::npos);
  // A done document says the turn can wrap up.
  RuntimeState done = st;
  done.plan["status"] = "done";
  EXPECT_TRUE(AgentContext::build_runtime(in, done).find("wrap up") != std::string::npos);

  std::string wrapped = AgentContext::wrap_user_turn(rt, "do the thing");
  EXPECT_TRUE(wrapped.find("<runtime>") != std::string::npos);
  EXPECT_TRUE(wrapped.find("do the thing") != std::string::npos);
  // Round trip: the stored text is what the user actually typed.
  EXPECT_EQ(AgentContext::unwrap_user_text(wrapped), std::string("do the thing"));
  // No runtime block -> no wrapper (a plain message stays plain).
  EXPECT_EQ(AgentContext::wrap_user_turn("", "hi"), std::string("hi"));
}

// ---------- 工具索引只列出存在的工具 ----------
FS_TEST(context_tool_index_matches_tools) {
  ContextInput in = make_ctx(ContextLang::kEn);
  std::string sys = AgentContext::build_system(in);
  EXPECT_TRUE(sys.find("read_file:") != std::string::npos);
  EXPECT_TRUE(sys.find("write_file:") != std::string::npos);
  // A tool that is not in the list must not appear in the index.
  EXPECT_TRUE(sys.find("terminal_exec") == std::string::npos);
  // The schemas themselves must NOT be duplicated into the prompt: they are
  // sent through the API's tools parameter, and duplicating them doubles the
  // largest fixed block.
  EXPECT_TRUE(sys.find("\"parameters\"") == std::string::npos);
}

// ---------- 自动模式：ask 不在工具里，并明确告知 ----------
FS_TEST(context_auto_mode_hides_ask) {
  ContextInput in = make_ctx(ContextLang::kEn);
  in.auto_mode = true;
  in.allow_ask = false;
  in.disabled_tools.push_back("ask");
  std::string sys = AgentContext::build_system(in);
  EXPECT_TRUE(sys.find("Auto mode") != std::string::npos);
  EXPECT_TRUE(sys.find("ask is unavailable") != std::string::npos);
  EXPECT_TRUE(sys.find("Disabled (do not call): ask") != std::string::npos);
}

// ---------- 项目约定 / 长期记忆 / 环境 ----------
FS_TEST(context_project_and_env_sections) {
  ContextInput in = make_ctx(ContextLang::kEn);
  in.project_instructions = "Use tabs. Never touch generated/.";
  in.instructions_source = "AGENTS.md";
  in.memory = Json{{"tone", "concise"}};
  in.cli_tools = Json{{"git", "C:/git/git.exe"}};
  in.env_names = Json::array({"NODE_ENV"});
  in.git = Json{{"branch", "main"}, {"dirty", true}};
  std::string sys = AgentContext::build_system(in);
  EXPECT_TRUE(sys.find("Use tabs") != std::string::npos);
  EXPECT_TRUE(sys.find("AGENTS.md") != std::string::npos);
  EXPECT_TRUE(sys.find("concise") != std::string::npos);
  EXPECT_TRUE(sys.find("C:/git/git.exe") != std::string::npos);
  EXPECT_TRUE(sys.find("NODE_ENV") != std::string::npos);
  EXPECT_TRUE(sys.find("main (dirty)") != std::string::npos);
}

// ---------- 子代理上下文：角色/任务，且不要求思考语言 ----------
FS_TEST(context_subagent_role) {
  ContextInput in = make_ctx(ContextLang::kEn);
  in.role = "subagent";
  in.subagent_kind = "local-research";
  in.subagent_task = "find the auth bug";
  std::string sys = AgentContext::build_system(in);
  EXPECT_TRUE(sys.find("subagent") != std::string::npos);
  EXPECT_TRUE(sys.find("find the auth bug") != std::string::npos);
  EXPECT_TRUE(sys.find("return only a summary") != std::string::npos);
}

// ===========================================================================
// provider adapter
// ===========================================================================

// ---------- OpenAI：tool 结果必须是 role:"tool" + tool_call_id ----------
FS_TEST(adapter_openai_tool_result_shape) {
  Json msgs = Json::array({
      Json{{"role", "system"}, {"content", "sys"}},
      Json{{"role", "user"}, {"content", "read a"}},
      Json{{"role", "assistant"},
           {"content", ""},
           {"toolCalls", Json::array({Json{{"id", "c1"},
                                           {"name", "read_file"},
                                           {"args", Json{{"path", "a"}}}}})}},
      Json{{"role", "tool"},
           {"toolCallId", "c1"},
           {"name", "read_file"},
           {"content", "{\"ok\":true}"}},
  });
  prompt::Adapted a = prompt::adapt(ApiKind::kOpenAIChat, msgs, Json::array());
  ASSERT_TRUE(a.messages.is_array());
  EXPECT_EQ((int)a.messages.size(), 4);
  EXPECT_EQ(a.messages[2]["role"].get<std::string>(), std::string("assistant"));
  EXPECT_TRUE(a.messages[2].contains("tool_calls"));
  EXPECT_EQ(a.messages[2]["tool_calls"][0]["id"].get<std::string>(), std::string("c1"));
  // arguments must be a STRING in OpenAI's wire format
  EXPECT_TRUE(a.messages[2]["tool_calls"][0]["function"]["arguments"].is_string());
  EXPECT_EQ(a.messages[3]["role"].get<std::string>(), std::string("tool"));
  EXPECT_EQ(a.messages[3]["tool_call_id"].get<std::string>(), std::string("c1"));
}

// ---------- Anthropic：tool_use / tool_result blocks + 顶层 system ----------
FS_TEST(adapter_anthropic_blocks) {
  Json msgs = Json::array({
      Json{{"role", "system"}, {"content", "sys"}},
      Json{{"role", "user"}, {"content", "read a"}},
      Json{{"role", "assistant"},
           {"content", "on it"},
           {"toolCalls", Json::array({Json{{"id", "tu_1"},
                                           {"name", "read_file"},
                                           {"args", Json{{"path", "a"}}}}})}},
      Json{{"role", "tool"}, {"toolCallId", "tu_1"}, {"name", "read_file"},
           {"content", "ok"}},
  });
  prompt::Adapted a = prompt::adapt(ApiKind::kAnthropic, msgs, Json::array());
  ASSERT_TRUE(a.system.is_string());
  EXPECT_EQ(a.system.get<std::string>(), std::string("sys"));
  // system must NOT also be a message
  for (auto& m : a.messages) {
    EXPECT_NE(m["role"].get<std::string>(), std::string("system"));
  }
  ASSERT_TRUE(a.messages.size() >= 3);
  bool saw_tool_use = false, saw_tool_result = false;
  for (auto& m : a.messages) {
    for (auto& b : m["content"]) {
      if (b.value("type", "") == "tool_use") {
        saw_tool_use = true;
        EXPECT_EQ(b["input"]["path"].get<std::string>(), std::string("a"));
      }
      if (b.value("type", "") == "tool_result") {
        saw_tool_result = true;
        EXPECT_EQ(b["tool_use_id"].get<std::string>(), std::string("tu_1"));
      }
    }
  }
  EXPECT_TRUE(saw_tool_use);
  EXPECT_TRUE(saw_tool_result);
}

// ---------- Anthropic 的相邻同角色合并 + 首条必须是 user ----------
FS_TEST(adapter_anthropic_repairs_history) {
  Json msgs = Json::array({
      Json{{"role", "system"}, {"content", "sys"}},
      Json{{"role", "user"}, {"content", "first"}},
      Json{{"role", "user"}, {"content", "second"}},
  });
  prompt::Adapted a = prompt::adapt(ApiKind::kAnthropic, msgs, Json::array());
  EXPECT_EQ((int)a.messages.size(), 1);  // merged
  EXPECT_EQ(a.messages[0]["role"].get<std::string>(), std::string("user"));

  // History that starts with an assistant turn gets a neutral lead-in instead
  // of being rejected by the API.
  Json m2 = Json::array({
      Json{{"role", "assistant"}, {"content", "I was saying..."}},
      Json{{"role", "user"}, {"content", "go on"}},
  });
  prompt::Adapted b = prompt::adapt(ApiKind::kAnthropic, m2, Json::array());
  EXPECT_TRUE(b.messages.size() >= 2);
  EXPECT_EQ(b.messages[0]["role"].get<std::string>(), std::string("user"));
}

// ---------- 工具定义：三种形态 ----------
FS_TEST(adapter_tool_definitions) {
  Json tools = Json::array({Json{{"name", "read_file"},
                                 {"description", "read"},
                                 {"parameters", Json{{"type", "object"}}}}});
  Json oa = prompt::adapt_tools(ApiKind::kOpenAIChat, tools);
  EXPECT_EQ(oa[0]["type"].get<std::string>(), std::string("function"));
  EXPECT_TRUE(oa[0].contains("function"));
  Json an = prompt::adapt_tools(ApiKind::kAnthropic, tools);
  EXPECT_TRUE(an[0].contains("input_schema"));
  EXPECT_FALSE(an[0].contains("function"));
  Json ra = prompt::adapt_tools(ApiKind::kOpenAIResponse, tools);
  EXPECT_EQ(ra[0]["name"].get<std::string>(), std::string("read_file"));
  EXPECT_TRUE(ra[0].contains("parameters"));
}

// ---------- tool call 归一化：字符串参数解析失败不能当空对象执行 ----------
FS_TEST(adapter_normalize_tool_call) {
  Json wire = Json::object();
  wire["id"] = "call_9";
  wire["function"] = Json{{"name", "write_file"}, {"arguments", "{\"path\":\"a\"}"}};
  Json n = prompt::normalize_tool_call(wire, "fb");
  EXPECT_EQ(n["id"].get<std::string>(), std::string("call_9"));
  EXPECT_EQ(n["name"].get<std::string>(), std::string("write_file"));
  EXPECT_EQ(n["args"]["path"].get<std::string>(), std::string("a"));
  EXPECT_FALSE(n.contains("argsParseError"));

  Json bad = Json::object();
  bad["id"] = "c2";
  bad["function"] = Json{{"name", "write_file"}, {"arguments", "{not json"}};
  Json nb = prompt::normalize_tool_call(bad, "fb");
  EXPECT_TRUE(nb.contains("argsParseError"));
  EXPECT_TRUE(nb["args"].is_object());
  // missing id gets the fallback so the result can still be matched
  Json noid = Json{{"name", "read_file"}, {"args", Json::object()}};
  EXPECT_EQ(prompt::normalize_tool_call(noid, "fb").value("id", ""), std::string("fb"));
}

// ===========================================================================
// HistoryBuilder
// ===========================================================================

// ---------- 工具结果必须按顺序回到历史里 ----------
FS_TEST(history_replays_tool_results) {
  HistoryBuilder::Options opts;
  opts.system_prefix = "sys";
  HistoryBuilder hb(opts);
  Json full = Json::object();
  Json turns = Json::array();
  turns.push_back(Json{{"role", "user"}, {"promptRaw", "read a.txt"}});
  Json asst = Json::object();
  asst["role"] = "assistant";
  asst["streamSummary"] = "reading";
  asst["toolCalls"] = Json::array({Json{{"id", "c1"},
                                         {"name", "read_file"},
                                         {"args", Json{{"path", "a.txt"}}},
                                         {"ok", true},
                                         {"summary", "3 lines"},
                                         {"detail", "line1\nline2\nline3"}}});
  turns.push_back(asst);
  full["turns"] = turns;
  HistoryResult hr = hb.build_neutral(full);
  // system + user + assistant + tool
  ASSERT_TRUE(hr.messages.size() >= 4);
  EXPECT_EQ(hr.messages[0]["role"].get<std::string>(), std::string("system"));
  EXPECT_EQ(hr.messages[2]["role"].get<std::string>(), std::string("assistant"));
  EXPECT_TRUE(hr.messages[2].contains("toolCalls"));
  EXPECT_EQ(hr.messages[3]["role"].get<std::string>(), std::string("tool"));
  EXPECT_EQ(hr.messages[3]["toolCallId"].get<std::string>(), std::string("c1"));
  EXPECT_TRUE(hr.messages[3]["content"].get<std::string>().find("line2") !=
              std::string::npos);
}

// A tool result is PROJECTED on replay: the fields that let the model continue
// survive, the bulk (a whole file body, an edit's before/after text, a long
// directory listing) does not. This is what keeps a long session's context from
// being dominated by what tools returned earlier.
FS_TEST(history_projects_tool_results_on_replay) {
  // edit_file: keep path/hash and the count of changes, drop before/after text.
  Json edit = Json{{"ok", true},
                   {"path", "src/a.cpp"},
                   {"hash", "abc123"},
                   {"totalLines", 120}};
  edit["changes"] = Json::array({Json{{"start", 4},
                                      {"end", 6},
                                      {"before", std::string(1500, 'x')},
                                      {"after", std::string(1500, 'y')}}});
  std::string p = HistoryBuilder::project_tool_result("edit_file", edit, "");
  EXPECT_TRUE(p.find("src/a.cpp") != std::string::npos);
  EXPECT_TRUE(p.find("abc123") != std::string::npos);
  EXPECT_TRUE(p.find("\"count\":1") != std::string::npos ||
              p.find("\"count\": 1") != std::string::npos);
  EXPECT_TRUE(p.find(std::string(1500, 'x')) == std::string::npos);

  // read_file: keep the identity + a bounded sample of the body.
  Json rd = Json{{"ok", true},
                 {"path", "big.txt"},
                 {"hash", "h1"},
                 {"totalLines", 9000},
                 {"content", std::string(5000, 'a')}};
  std::string pr = HistoryBuilder::project_tool_result("read_file", rd, "");
  EXPECT_TRUE(pr.find("big.txt") != std::string::npos);
  EXPECT_TRUE(pr.find("h1") != std::string::npos);
  EXPECT_TRUE(pr.size() < 2000);  // a 5000-char body must not come back whole

  // A failure keeps its reason.
  Json bad = Json{{"ok", false}, {"error", "file not found"}, {"errCode", "not-found"}};
  std::string pb = HistoryBuilder::project_tool_result("read_file", bad, "");
  EXPECT_TRUE(pb.find("file not found") != std::string::npos);

  // An unknown tool with a plain-text detail still survives (bounded).
  std::string pu = HistoryBuilder::project_tool_result(
      "some_mcp_tool", Json::object(), std::string(3000, 'z'));
  EXPECT_TRUE(pu.find(std::string(3000, 'z')) == std::string::npos);
  EXPECT_TRUE(pu.size() > 10);
}

// ---------- 预算裁剪保留最新，而不是丢掉最新的 ----------
// ---------- 不设窗口：所有轮次都回放（token 预算不再是触发器）----------
//
// The old behaviour folded everything beyond `recent_turns` / the token budget,
// which compacted a long session on EVERY turn no matter the water level. The only
// automatic trigger is the water level now; without a context limit and without a
// forced compaction, the whole transcript is replayed.
FS_TEST(history_replays_every_turn_without_a_water_level) {
  HistoryBuilder::Options opts;
  opts.system_prefix = "sys";
  HistoryBuilder hb(opts);
  Json full = Json::object();
  Json turns = Json::array();
  for (int i = 0; i < 6; ++i) {
    Json u = Json::object();
    u["role"] = "user";
    u["promptRaw"] = "question number " + std::to_string(i) +
                     " with enough words to cost tokens";
    turns.push_back(u);
    Json a = Json::object();
    a["role"] = "assistant";
    a["streamSummary"] = "answer " + std::to_string(i);
    turns.push_back(a);
  }
  full["turns"] = turns;
  HistoryResult hr = hb.build_neutral(full);
  EXPECT_FALSE(hr.compressed);   // no water level, no forced compaction
  // Every turn is there - including the oldest, which the budget used to drop.
  std::string all;
  for (auto& m : hr.messages) all += jsonutil::get<std::string>(m, "content", "");
  EXPECT_TRUE(all.find("answer 0") != std::string::npos);
  EXPECT_TRUE(all.find("answer 5") != std::string::npos);
  EXPECT_TRUE(all.find("compacted") == std::string::npos);
}

// ---------- 遗忘 / keyPoints 折叠 ----------
FS_TEST(history_forget_and_keypoints) {
  HistoryBuilder::Options opts;
  opts.system_prefix = "sys";
  // Compaction is now triggered by the WATER LEVEL (or "compact now"), so ask for
  // it explicitly. keep_turns=1 keeps one turn in full; everything older folds into
  // the summary, which is where keyPoints surface.
  opts.force_compress = true;
  opts.keep_turns = 1;
  HistoryBuilder hb(opts);
  Json turns = Json::array();
  for (int i = 0; i < 4; ++i) {
    Json u = Json::object();
    u["role"] = "user";
    u["promptRaw"] = "q" + std::to_string(i) + " padded with words";
    if (i == 1) u["forgotten"] = true;
    if (i == 0) u["keyPoints"] = Json::array({"point-alpha"});
    turns.push_back(u);
  }
  Json full = Json::object();
  full["turns"] = turns;
  HistoryResult hr = hb.build_neutral(full);
  EXPECT_TRUE(hr.compressed);
  bool has_alpha = false, has_forgotten = false;
  for (auto& m : hr.messages) {
    std::string c = jsonutil::get<std::string>(m, "content", "");
    if (c.find("point-alpha") != std::string::npos) has_alpha = true;
    if (c.find("q1 padded") != std::string::npos) has_forgotten = true;
  }
  EXPECT_TRUE(has_alpha);
  EXPECT_FALSE(has_forgotten);
}
