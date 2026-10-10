// provider_runtime.cpp - usage recording + retry + failure counting
#include "provider_runtime.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <ctime>
#endif

namespace fs {

namespace {
WallClock g_default_clock;
}  // namespace

ProviderRuntime::ProviderRuntime(const ProviderConn& conn, const NetClient* net,
                                 UsageStore* usage, const Clock* clock)
    : conn_(conn), net_(net), usage_(usage), clock_(clock) {
  if (!clock_) clock_ = &g_default_clock;
}

bool ProviderRuntime::should_retry(const FsResult& r, const ChatResponse& resp,
                                   int attempt) const {
  if (attempt >= policy_.max_retries) return false;
  if (r.code == ErrCode::kTimeout) return policy_.retry_on_timeout;
  if (r.code == ErrCode::kNetwork) return policy_.retry_on_network;
  if (r.code == ErrCode::kRemote && resp.http_status >= 500 && resp.http_status < 600) {
    return policy_.retry_on_5xx;
  }
  return false;
}

void ProviderRuntime::record_usage(const ChatRequest& req, const ChatResponse& resp,
                                   bool ok) {
  if (!usage_) return;
  UsageRow row;
  row.ts = clock_->now_ms();
  row.provider = conn_.id;
  row.model = req.model;
  row.in = resp.in_tokens;
  row.out = resp.out_tokens;
  row.cache = resp.cache_tokens;
  row.cost = resp.cost;
  row.cost_kind = resp.cost_kind;
  row.latency_ms = resp.latency_ms;
  row.tool_calls = static_cast<int64_t>(resp.tool_calls.size());
  row.error = ok ? "" : resp.error;
  usage_->record(row);
}

namespace {
void sleep_ms(int64_t ms) {
  if (ms <= 0) return;
#if defined(_WIN32)
  Sleep(static_cast<DWORD>(ms));
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
#endif
}
}  // namespace

FsResult ProviderRuntime::call(const ChatRequest& req, ChatResponse* out,
                               const StreamDeltaFn& on_delta) {
  if (!out) return FsResult::error(ErrCode::kInvalidArg, "null response");
  ProviderClient client(conn_, net_);
  ++failures_.total_calls;
  for (int attempt = 0;; ++attempt) {
    ChatResponse attempt_out;
    FsResult r = client.chat(req, &attempt_out, on_delta);
    record_usage(req, attempt_out, r.ok() && attempt_out.error.empty());
    if (r.ok() && attempt_out.error.empty()) {
      failures_.consecutive_failures = 0;
      *out = std::move(attempt_out);
      return r;
    }
    ++failures_.consecutive_failures;
    if (!should_retry(r, attempt_out, attempt)) {
      out->error = attempt_out.error.empty() ? r.message : attempt_out.error;
      return r.ok() ? r : FsResult::error(r.code, out->error);
    }
    // exponential backoff (only sleeps when attempt>0; tests use FakeClock and small retries)
    if (attempt > 0) sleep_ms(policy_.backoff_ms * (1 << (attempt - 1)));
  }
}

}  // namespace fs