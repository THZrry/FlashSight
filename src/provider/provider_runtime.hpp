// provider_runtime.hpp - ProviderRuntime：用量记录 + 超时/重试/失败计数（roadmap 20-provider 流式与用量）
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "provider_client.hpp"
#include "provider_store.hpp"
#include "provider_types.hpp"
#include "usage_store.hpp"

namespace fs {

// 失败计数（refreshConfig 清零；P3 状态机接行为）
struct FailCounters {
  int64_t consecutive_failures = 0;
  int64_t total_calls = 0;
};

// 超时/重试策略（定死可测；roadmap: 策略定死并单测）
struct RetryPolicy {
  // A flaky relay is the normal case this exists for: three retries, 1s/2s/4s.
  // Attempts that already emitted text are NOT retried (the visible half would
  // be duplicated) - see ProviderRuntime::call.
  int max_retries = 3;        // 首次 + 3 次重试
  int64_t backoff_ms = 1000;  // 退避基数（指数：1000, 2000, 4000）
  int64_t timeout_ms = 120000;
  bool retry_on_timeout = true;
  bool retry_on_network = true;
  bool retry_on_5xx = true;
};

class ProviderRuntime {
 public:
  ProviderRuntime(const ProviderConn& conn, const NetClient* net,
                  UsageStore* usage, const Clock* clock);

  // 发起一次调用：自动用量记录 + 重试/失败计数 + 流式回调
  // 返回：成功时 out 填好；失败时 out->error 非空
  FsResult call(const ChatRequest& req, ChatResponse* out,
                const StreamDeltaFn& on_delta = nullptr);

  // 失败计数查询/清零（refreshConfig 清）
  const FailCounters& failures() const { return failures_; }
  void reset_failures() { failures_.consecutive_failures = 0; }
  RetryPolicy& retry_policy() { return policy_; }
  const RetryPolicy& retry_policy() const { return policy_; }

 private:
  ProviderConn conn_;
  const NetClient* net_;
  UsageStore* usage_;
  const Clock* clock_;
  RetryPolicy policy_;
  FailCounters failures_;

  // 是否应重试
  bool should_retry(const FsResult& r, const ChatResponse& resp, int attempt) const;
  void record_usage(const ChatRequest& req, const ChatResponse& resp, bool ok);
};

}  // namespace fs