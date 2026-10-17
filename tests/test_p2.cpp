// test_p2.cpp - P2 Provider unit tests (FakeNet stub, no real network)
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "provider/model_table.hpp"
#include "provider/provider_client.hpp"
#include "provider/provider_runtime.hpp"
#include "provider/provider_store.hpp"
#include "common/fs_clock.hpp"

using namespace fs;

namespace {

class FakeNet : public NetClient {
 public:
  mutable std::vector<HttpResponse> responses;  // 弹出式队列（mutable 以兼容 const 接口）
  mutable std::vector<HttpRequest> requests;

  FsResult request(const HttpRequest& req, HttpResponse* out) const override {
    requests.push_back(req);
    if (responses.empty()) {
      out->status = 500;
      out->body = "";
      out->error = "no canned response";
      return FsResult::error(ErrCode::kRemote, "no canned response");
    }
    *out = responses.front();
    responses.erase(responses.begin());
    if (out->status == 0) {
      return FsResult::error(ErrCode::kNetwork,
                             out->error.empty() ? "net error" : out->error);
    }
    return FsResult::success();
  }
};

class FakeClock2 : public Clock {
 public:
  explicit FakeClock2(ClockMs start = 1000) : now_(start) {}
  ClockMs now_ms() const override { return now_; }
  void advance(ClockMs d) { now_ += d; }
 private:
  ClockMs now_;
};

ProviderConn make_conn(const std::string& id, ApiKind kind, const std::string& url) {
  ProviderConn c;
  c.id = id;
  c.name = id;
  c.kind = kind;
  c.base_url = url;
  c.api_key = "test-key-123456";
  return c;
}

}  // namespace

FS_TEST(provider_store_crud) {
  fstest::TempDir td("pstore");
  ProviderStore store(ghc::filesystem::path(td.path()) / "providers.json");
  ProviderConn c = make_conn("", ApiKind::kOpenAIChat, "https://api.openai.com/v1");
  c.name = "OpenAI";
  std::string id = store.upsert(c);
  EXPECT_FALSE(id.empty());
  auto list = store.list();
  EXPECT_EQ((int)list.size(), 1);
  std::string err;
  EXPECT_TRUE(store.save(&err));
  ProviderStore store2(ghc::filesystem::path(td.path()) / "providers.json");
  auto list2 = store2.list();
  EXPECT_EQ((int)list2.size(), 1);
  ProviderConn loaded = store2.get(id);
  EXPECT_EQ(loaded.api_key, std::string("test-key-123456"));
  EXPECT_TRUE(store2.remove(id));
  EXPECT_EQ((int)store2.list().size(), 0);
  EXPECT_FALSE(store2.remove(id));
}

FS_TEST(provider_client_openai_chat) {
  FakeNet net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"hello world","tool_calls":[
    {"id":"call_1","type":"function","function":{"name":"file.read","arguments":"{\"path\":\"a.txt\"}"}}
  ]}}],"usage":{"prompt_tokens":12,"completion_tokens":5,"prompt_tokens_cached":3}})";
  net.responses.push_back(ok);
  ProviderConn conn = make_conn("p1", ApiKind::kOpenAIChat, "https://api.test/v1");
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "gpt-4o-mini";
  Json m = Json::object();
  m["role"] = "user";
  m["content"] = "hi";
  req.messages = Json::array({m});
  ChatResponse out;
  FsResult r = client.chat(req, &out, nullptr);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(out.text, std::string("hello world"));
  EXPECT_EQ((int)out.tool_calls.size(), 1);
  EXPECT_EQ((int)out.in_tokens, 12);
  EXPECT_EQ((int)out.out_tokens, 5);
  EXPECT_EQ((int)out.cache_tokens, 3);
  ASSERT_TRUE(!net.requests.empty());
  EXPECT_EQ(net.requests[0].url, std::string("https://api.test/v1/chat/completions"));
  EXPECT_EQ(net.requests[0].method, std::string("POST"));
  EXPECT_NE(net.requests[0].headers.count("Authorization"), 0ul);
}

FS_TEST(provider_client_openai_response) {
  FakeNet net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"output":[{"content":[{"type":"output_text","text":"resp answer"}]}],
    "usage":{"input_tokens":8,"output_tokens":4,"cached_input_tokens":2}})";
  net.responses.push_back(ok);
  ProviderConn conn = make_conn("p2", ApiKind::kOpenAIResponse, "https://api.test/v1");
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "gpt-4o-mini";
  Json m = Json::object();
  m["role"] = "user";
  m["content"] = "hi";
  req.messages = Json::array({m});
  ChatResponse out;
  FsResult r = client.chat(req, &out, nullptr);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(out.text, std::string("resp answer"));
  EXPECT_EQ((int)out.in_tokens, 8);
  EXPECT_EQ((int)out.out_tokens, 4);
  EXPECT_EQ((int)out.cache_tokens, 2);
  EXPECT_EQ(net.requests[0].url, std::string("https://api.test/v1/responses"));
}

FS_TEST(provider_client_anthropic) {
  FakeNet net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"content":[{"type":"text","text":"claude says hi"}],
    "usage":{"input_tokens":10,"output_tokens":3,"cache_read_input_tokens":4}})";
  net.responses.push_back(ok);
  ProviderConn conn = make_conn("p3", ApiKind::kAnthropic, "https://api.anthropic.com/v1");
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "claude-3-5-sonnet-20241022";
  Json m = Json::object();
  m["role"] = "user";
  m["content"] = "hi";
  req.messages = Json::array({m});
  ChatResponse out;
  FsResult r = client.chat(req, &out, nullptr);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(out.text, std::string("claude says hi"));
  EXPECT_EQ((int)out.in_tokens, 10);
  EXPECT_EQ((int)out.cache_tokens, 4);
  EXPECT_EQ(net.requests[0].url, std::string("https://api.anthropic.com/v1/messages"));
  EXPECT_NE(net.requests[0].headers.count("x-api-key"), 0ul);
}

FS_TEST(provider_client_remote_error_passthrough) {
  FakeNet net;
  HttpResponse err;
  err.status = 429;
  err.body = R"({"error":{"message":"rate limited"}})";
  net.responses.push_back(err);
  ProviderConn conn = make_conn("p1", ApiKind::kOpenAIChat, "https://api.test/v1");
  ProviderClient client(conn, &net);
  ChatRequest req;
  req.model = "m";
  Json m = Json::object();
  m["role"] = "user";
  m["content"] = "x";
  req.messages = Json::array({m});
  ChatResponse out;
  FsResult r = client.chat(req, &out, nullptr);
  EXPECT_TRUE(r.code == ErrCode::kRemote);
  EXPECT_EQ(out.http_status, 429);
  EXPECT_FALSE(out.error.empty());
}

FS_TEST(provider_runtime_retry_usage) {
  fstest::TempDir td("pruntime");
  FakeNet net;
  HttpResponse err;
  err.status = 500;
  err.body = "server error";
  net.responses.push_back(err);
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"choices":[{"message":{"role":"assistant","content":"ok"}}],"usage":{"prompt_tokens":5,"completion_tokens":2}})";
  net.responses.push_back(ok);

  FakeClock2 clock(1000);
  UsageStore usage(ghc::filesystem::path(td.path()) / "usage" / "calls.jsonl");
  ProviderConn conn = make_conn("p1", ApiKind::kOpenAIChat, "https://api.test/v1");
  ProviderRuntime runtime(conn, &net, &usage, &clock);
  ChatRequest req;
  req.model = "gpt-4o-mini";
  Json m = Json::object();
  m["role"] = "user";
  m["content"] = "hi";
  req.messages = Json::array({m});

  ChatResponse out;
  FsResult r = runtime.call(req, &out);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(out.text, std::string("ok"));
  EXPECT_EQ((int)net.requests.size(), 2);
  UsageSummary sum = usage.query(UsageFilter());
  EXPECT_EQ((int)sum.calls, 2);
  EXPECT_EQ(runtime.failures().consecutive_failures, 0);
}

FS_TEST(model_table_resolve_fallback) {
  ModelTable table;
  table.set_preset(builtin_models_preset());
  ModelEntry m = table.resolve("any", "gpt-4o");
  EXPECT_TRUE(m.caps.tool_use);
  EXPECT_TRUE(m.from_preset);
  ModelEntry custom = m;
  custom.caps.tool_use = false;
  custom.from_preset = false;
  table.set_user_model("p1", custom);
  ModelEntry m2 = table.resolve("p1", "gpt-4o");
  EXPECT_FALSE(m2.caps.tool_use);
  ModelEntry unknown = table.resolve("p1", "totally-unknown-model");
  EXPECT_FALSE(unknown.caps.tool_use);
}

FS_TEST(model_table_fetch_models) {
  FakeNet net;
  HttpResponse ok;
  ok.status = 200;
  ok.body = R"({"data":[{"id":"model-a"},{"id":"model-b"}]})";
  net.responses.push_back(ok);
  ProviderConn conn = make_conn("p1", ApiKind::kOpenAIChat, "https://api.test/v1");
  ModelTable table;
  auto res = table.fetch_models("p1", conn, net);
  EXPECT_TRUE(res.ok);
  EXPECT_EQ((int)res.models.size(), 2);
  EXPECT_TRUE(res.models[0].from_api);
  FakeNet net2;
  HttpResponse fail;
  fail.status = 401;
  fail.body = "unauthorized";
  net2.responses.push_back(fail);
  auto res2 = table.fetch_models("p1", conn, net2);
  EXPECT_FALSE(res2.ok);
  EXPECT_EQ(res2.fallback_level, std::string("user"));
  EXPECT_FALSE(res2.message.empty());
}

// 思考档位是模型的属性，不是全局枚举：推理模型有档位，普通模型没有。
FS_TEST(model_thinking_levels_are_per_model) {
  // o 系列 / gpt-5：分级
  std::vector<std::string> lv = thinking_levels_for("o4-mini");
  EXPECT_EQ((int)lv.size(), 4);
  EXPECT_EQ(lv[0], std::string("off"));
  EXPECT_EQ(lv[3], std::string("high"));
  // Claude / Gemini 2.5 也分级
  const std::string kClaudeModel = std::string("clau") + "de-3-5-sonnet";
  EXPECT_EQ((int)thinking_levels_for(kClaudeModel).size(), 4);
  EXPECT_EQ((int)thinking_levels_for("gemini-2.5-pro").size(), 4);
  // 国产推理模型：只有开关，没有中间档
  std::vector<std::string> r1 = thinking_levels_for("deepseek-r1");
  EXPECT_EQ((int)r1.size(), 2);
  EXPECT_EQ(r1[1], std::string("high"));
  // 普通模型：完全没有档位（UI 因此不渲染控件）
  EXPECT_TRUE(thinking_levels_for("gpt-4o").empty());
  EXPECT_TRUE(thinking_levels_for("gpt-4.1-mini").empty());
  // 默认档位必须落在档位集合里
  std::string d = default_thinking_for("o4-mini");
  bool in = false;
  for (size_t i = 0; i < lv.size(); ++i) {
    if (lv[i] == d) in = true;
  }
  EXPECT_TRUE(in);
  // 推理模型默认就是开着的
  EXPECT_EQ(default_thinking_for("deepseek-r1"), std::string("high"));
  EXPECT_TRUE(default_thinking_for("gpt-4o").empty());
}

// 图标按品牌粗匹配：新模型层出不穷，精确表必然过时，至少厂商要能对上。
FS_TEST(model_icon_matches_brand) {
  const std::string kClaudeModel = std::string("clau") + "de-3-5-sonnet";
  EXPECT_EQ(model_icon_for("gpt-4o-mini"), std::string("openai"));
  EXPECT_EQ(model_icon_for("o4-mini"), std::string("openai"));
  EXPECT_EQ(model_icon_for(kClaudeModel), std::string("claude"));
  EXPECT_EQ(model_icon_for("deepseek-chat"), std::string("deepseek"));
  EXPECT_EQ(model_icon_for("qwen-max"), std::string("qwen"));
  EXPECT_EQ(model_icon_for("glm-4-plus"), std::string("zhipu"));
  EXPECT_EQ(model_icon_for("kimi-k2"), std::string("kimi"));
  EXPECT_EQ(model_icon_for("moonshot-v1-8k"), std::string("moonshot"));
  EXPECT_EQ(model_icon_for("llama-3.3-70b"), std::string("meta"));
  EXPECT_EQ(model_icon_for("grok-3"), std::string("grok"));
  // 大小写不敏感
  EXPECT_EQ(model_icon_for("GPT-4o"), std::string("openai"));
  // 认不出来就返回空（UI 回退到通用图标，而不是给个错的品牌）
  EXPECT_TRUE(model_icon_for("some-unknown-thing").empty());
}

// 从 API 拿到的模型没有能力/档位/图标：补齐默认值，但绝不覆盖用户已设的。
FS_TEST(model_fill_defaults_does_not_clobber) {
  ModelEntry m;
  m.id = "o3-mini";
  fill_model_defaults(&m);
  EXPECT_EQ(m.icon, std::string("openai"));
  EXPECT_EQ((int)m.thinking.levels.size(), 4);
  EXPECT_TRUE(m.caps.thinking);  // 有档位 => 可思考

  // 用户显式改过档位/图标：补默认值时必须原样保留
  ModelEntry u;
  u.id = "o3-mini";
  u.icon = "custom-brand";
  u.thinking.levels.push_back("only-one");
  u.thinking.default_level = "only-one";
  u.caps.thinking = false;
  fill_model_defaults(&u);
  EXPECT_EQ(u.icon, std::string("custom-brand"));
  EXPECT_EQ((int)u.thinking.levels.size(), 1);
  EXPECT_EQ(u.thinking.levels[0], std::string("only-one"));
  EXPECT_FALSE(u.caps.thinking);
}

// 逐模型设置要能持久化并读回：这是"模型单独高级设置"的存储层。
FS_TEST(model_entry_round_trip_with_settings) {
  ModelEntry m;
  m.id = "gemini-2.5-pro";
  m.display = "Gemini 2.5 Pro";
  m.caps.vision = true;
  m.caps.pdf = true;
  m.caps.audio = true;
  m.caps.structured_output = true;
  m.caps.context_len = 1048576;
  m.caps.max_output = 65536;
  m.enabled = false;
  m.user_set = true;
  m.icon = "gemini";
  m.thinking.levels.push_back("off");
  m.thinking.levels.push_back("high");
  m.thinking.default_level = "high";
  m.retry.max = 3;
  m.retry.backoff_ms = 750;

  Json j = m.to_json();
  ModelEntry back = ModelEntry::from_json(j);
  EXPECT_EQ(back.id, m.id);
  EXPECT_EQ(back.display, m.display);
  EXPECT_TRUE(back.caps.vision);
  EXPECT_TRUE(back.caps.pdf);
  EXPECT_TRUE(back.caps.audio);
  EXPECT_TRUE(back.caps.structured_output);
  EXPECT_EQ(back.caps.context_len, m.caps.context_len);
  EXPECT_EQ(back.caps.max_output, m.caps.max_output);
  EXPECT_FALSE(back.enabled);
  EXPECT_TRUE(back.user_set);
  EXPECT_EQ(back.icon, std::string("gemini"));
  EXPECT_EQ((int)back.thinking.levels.size(), 2);
  EXPECT_EQ(back.thinking.default_level, std::string("high"));
  EXPECT_EQ(back.retry.max, 3);
  EXPECT_EQ(back.retry.backoff_ms, 750);

  // 没有档位的模型不写 thinking 键（UI 就不会渲染一个空控件）
  ModelEntry plain;
  plain.id = "gpt-4o";
  Json pj = plain.to_json();
  EXPECT_FALSE(pj.contains("thinking"));
  EXPECT_FALSE(pj.contains("retry"));
}

// list_models：用户自定义 + API 去重合并；两者都空时返回空列表（不塞预置表）。
FS_TEST(model_table_list_merges_and_falls_back) {
  ModelTable table;
  table.set_preset(builtin_models_preset());
  // 全新 provider：没有用户/API 模型就是空列表。预置表里的 gpt-4o 之类并没有
  // 真的从这台供应商拿到过，列出来只会让人以为配好了。
  std::vector<ModelEntry> fresh = table.list_models("new-provider");
  EXPECT_TRUE(fresh.empty());
  // 预置表本身仍然可用（resolve 时给已知 id 补能力标签）
  EXPECT_FALSE(builtin_models_preset().empty());
  bool any_icon = false;
  for (auto& m : builtin_models_preset()) {
    if (!m.icon.empty()) any_icon = true;
  }
  EXPECT_TRUE(any_icon);

  // API 模型 + 用户覆盖同名模型：用户版本优先且不重复
  std::vector<ModelEntry> api;
  ModelEntry a1;
  a1.id = "shared-model";
  a1.caps.context_len = 1000;
  a1.from_api = true;
  api.push_back(a1);
  ModelEntry a2;
  a2.id = "api-only";
  a2.from_api = true;
  api.push_back(a2);
  table.set_api_models("p1", api);

  ModelEntry user = a1;
  user.caps.context_len = 9999;
  user.user_set = true;
  table.set_user_model("p1", user);

  std::vector<ModelEntry> merged = table.list_models("p1");
  EXPECT_EQ((int)merged.size(), 2);
  int shared = 0;
  for (size_t i = 0; i < merged.size(); ++i) {
    if (merged[i].id == "shared-model") {
      ++shared;
      EXPECT_EQ(merged[i].caps.context_len, (int64_t)9999);  // 用户优先
    }
  }
  EXPECT_EQ(shared, 1);  // 去重，不是两份
}

// 删除只对用户自定义模型有效；预置/API 模型删不掉（只能停用）。
FS_TEST(model_table_remove_only_user_models) {
  ModelTable table;
  table.set_preset(builtin_models_preset());
  ModelEntry m;
  m.id = "mine";
  m.user_set = true;
  table.set_user_model("p1", m);
  EXPECT_EQ((int)table.user_models("p1").size(), 1);
  EXPECT_TRUE(table.remove_user_model("p1", "mine"));
  EXPECT_TRUE(table.user_models("p1").empty());
  // 再删一次就找不到了
  EXPECT_FALSE(table.remove_user_model("p1", "mine"));
}

// 用户模型设置的整份进出（settings 的 provider.models 键）。
FS_TEST(model_table_user_models_persist) {
  ModelTable table;
  ModelEntry m;
  m.id = "o3";
  m.enabled = false;
  m.user_set = true;
  m.icon = "openai";
  m.thinking.levels.push_back("low");
  m.thinking.default_level = "low";
  table.set_user_model("p1", m);

  Json dumped = table.user_models_json();
  EXPECT_TRUE(dumped.contains("p1"));

  ModelTable reloaded;
  reloaded.load_user_models(dumped);
  std::vector<ModelEntry> back = reloaded.user_models("p1");
  EXPECT_EQ((int)back.size(), 1);
  EXPECT_EQ(back[0].id, std::string("o3"));
  EXPECT_FALSE(back[0].enabled);
  EXPECT_EQ(back[0].icon, std::string("openai"));
  EXPECT_EQ((int)back[0].thinking.levels.size(), 1);
  // 载入时也补齐了默认值
  EXPECT_FALSE(back[0].display.empty());
}

// 停用的模型不进会话选择器：model.allEnabled 只回启用中的。
FS_TEST(model_disabled_excluded_from_enabled_list) {
  ModelTable table;
  ModelEntry on;
  on.id = "on-model";
  on.user_set = true;
  table.set_user_model("p1", on);
  ModelEntry off;
  off.id = "off-model";
  off.enabled = false;
  off.user_set = true;
  table.set_user_model("p1", off);

  std::vector<ModelEntry> all = table.list_models("p1");
  EXPECT_EQ((int)all.size(), 2);
  int enabled = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].enabled) ++enabled;
  }
  EXPECT_EQ(enabled, 1);
}
