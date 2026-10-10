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