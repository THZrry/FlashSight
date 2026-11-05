// provider_client.hpp - 模型调用客户端（OpenAI Chat / Response / Anthropic / custom）
// 网络走 NetClient（可桩），流式经 stream.delta 事件回调（20-provider.md 流式与用量）
#pragma once

#include <functional>
#include <string>

#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_net.hpp"
#include "provider_types.hpp"

namespace fs {

// 一次调用的输入（消息数组已按 provider 形态装配好）
struct ChatRequest {
  std::string model;
  Json messages = Json::array();  // provider 无关的中间格式（P3 HistoryBuilder 产出）
  Json tools = Json::array();     // 工具定义（provider 无关）
  // Anthropic keeps the system prompt out of `messages`; prompt::adapt() returns
  // it separately and it lands here. OpenAI-compatible shapes leave it empty.
  std::string system;
  // Reasoning effort for models that expose levels ("low"|"medium"|"high"|...).
  // Empty = do not send the field at all (a model without levels rejects it).
  std::string thinking;
  double temperature = 0.7;
  int64_t max_tokens = 4096;
  bool stream = true;
  // 会话亲和 id（kPlan 套餐自动带 x-opencode-session；opencode 实证：亲和/路由 id，非 token）
  std::string session_id;
  // Abort predicate, consulted between SSE chunks. Returning true stops the
  // transfer immediately, so 停止生成 during a long answer actually stops it -
  // the loop on its own can only park at a tool boundary, which never comes
  // during a plain streaming reply. Null = never cancel.
  std::function<bool()> cancel;
};

// 一次调用的输出（含用量）
struct ChatResponse {
  std::string text;        // 累积的文本输出
  Json tool_calls = Json::array();  // 工具调用（P3 解析执行）
  int64_t in_tokens = 0;
  int64_t out_tokens = 0;
  int64_t cache_tokens = 0;
  // Whether the endpoint actually REPORTED a cache figure. 0 without this flag
  // means "unknown", not "nothing was cached" - the UI shows "—" for unknown.
  bool cache_known = false;
  double cost = 0.0;
  std::string cost_kind = "unknown";  // "money"|"quota"|"unknown"
  int64_t latency_ms = 0;
  int http_status = 0;
  std::string error;                  // 空 = 成功
};

// 流式 delta 回调（text delta；UI P4 接，本阶段先推测试订阅者）
using StreamDeltaFn = std::function<void(const std::string& delta)>;

class ProviderClient {
 public:
  ProviderClient(const ProviderConn& conn, const NetClient* net);

  // 发起一次模型调用（按 conn.kind 选形态；stream=true 时边收边调 on_delta）
  FsResult chat(const ChatRequest& req, ChatResponse* out,
                const StreamDeltaFn& on_delta = nullptr) const;

  // testConnection：连通性 + 延迟 + 能力探测回填（provider.md §2）
  FsResult test_connection(ChatResponse* out) const;

  // 单测可覆盖的形态实现（内部用）
  FsResult chat_openai_chat(const ChatRequest& req, ChatResponse* out,
                            const StreamDeltaFn& on_delta) const;
  FsResult chat_openai_response(const ChatRequest& req, ChatResponse* out,
                                const StreamDeltaFn& on_delta) const;
  FsResult chat_anthropic(const ChatRequest& req, ChatResponse* out,
                          const StreamDeltaFn& on_delta) const;

 private:
  const ProviderConn& conn_;
  const NetClient* net_;

  // 解析 OpenAI Chat 的 tool_calls 与 usage（宽容）
  static void parse_openai_chat_response(const Json& body, ChatResponse* out);
  static void parse_anthropic_response(const Json& body, ChatResponse* out);
};

// 脱敏 trace（fetch_url 脱敏，此处给 key 掩码用）
std::string mask_secret(const std::string& s);

}  // namespace fs