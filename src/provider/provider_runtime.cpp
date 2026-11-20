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
  if (r.code == ErrCode::kCancelled) return false;
  // Retry is the DEFAULT for a failed request, not an opt-in for three codes.
  // A relay that answers 502, 429, a truncated stream or an empty_error reply is
  // exactly the unstable case the setting exists for; the old predicate only
  // retried timeout/network/5xx, so "3 retries" did nothing for most failures.
  //
  // Only genuinely terminal answers are excluded:
  //   * auth / bad-request / not-found (the same request cannot start working);
  //   * insufficient balance or quota (waiting cannot add credit);
  //   * host unreachable / DNS / offline (the relay is gone or there is no link).
  const int status = resp.http_status;
  if (status == 400 || status == 401 || status == 403 || status == 404 ||
      status == 422) {
    return false;
  }
  std::string msg = r.message;
  if (!resp.error.empty()) {
    if (!msg.empty()) msg += " ";
    msg += resp.error;
  }
  for (size_t i = 0; i < msg.size(); ++i) {
    if (msg[i] >= 'A' && msg[i] <= 'Z') msg[i] = static_cast<char>(msg[i] - 'A' + 'a');
  }
  static const char* kNoRetry[] = {
      "insufficient", "balance", "quota", "credit", "billing", "余额", "额度",
      "could not resolve", "name or service not known", "getaddrinfo",
      "connection refused", "no route to host", "network is unreachable",
      "failed to connect", "connect timeout", "offline", "dns",
  };
  for (size_t k = 0; k < sizeof(kNoRetry) / sizeof(kNoRetry[0]); ++k) {
    if (msg.find(kNoRetry[k]) != std::string::npos) return false;
  }
  // 429 and 5xx are explicitly retryable; so is any other remote/I-O/timeout
  // failure that produced no output (the caller checks `emitted` separately).
  (void)policy_.retry_on_timeout;
  (void)policy_.retry_on_network;
  (void)policy_.retry_on_5xx;
  return true;
}

void ProviderRuntime::record_usage(const ChatRequest& req, const ChatResponse& resp,
                                   bool ok) {
  if (!usage_) return;
  UsageRow row;
  row.ts = clock_->now_ms();
  row.sid = req.session_id;
  row.provider = conn_.id;
  row.model = req.model;
  row.in = resp.in_tokens;
  row.out = resp.out_tokens;
  row.cache = resp.cache_tokens;
  row.cache_known = resp.cache_known;
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
    // Has this attempt already shown the user something? A relay that dies
    // MID-STREAM leaves those bytes on screen, and replaying the reply from the
    // start would duplicate the visible half of the answer. Retry only while
    // nothing has been emitted.
    bool emitted = false;
    StreamDeltaFn attempt_delta;
    if (on_delta) {
      const StreamDeltaFn& cb = on_delta;
      attempt_delta = [&emitted, cb](const std::string& piece) {
        emitted = true;
        cb(piece);
      };
    }
    ChatRequest attempt_req = req;
    if (req.reasoning_delta) {
      const auto& cb = req.reasoning_delta;
      attempt_req.reasoning_delta = [&emitted, cb](const std::string& piece) {
        emitted = true;
        cb(piece);
      };
    }
    FsResult r = client.chat(attempt_req, &attempt_out, attempt_delta);
    record_usage(req, attempt_out, r.ok() && attempt_out.error.empty());    // A cancelled transfer (user 中止, or the no-progress watchdog) must not be
    // retried: the caller asked to stop, and retrying a hung relay just restarts
    // the same ten-minute wait.
    if (req.cancel && req.cancel()) {
      out->error = attempt_out.error.empty()
                       ? std::string("request cancelled or made no progress")
                       : attempt_out.error;
      return r.ok() ? FsResult::error(ErrCode::kCancelled, out->error) : r;
    }
    if (r.ok() && attempt_out.error.empty()) {
      failures_.consecutive_failures = 0;
      *out = std::move(attempt_out);
      return r;
    }
    ++failures_.consecutive_failures;
    if (!should_retry(r, attempt_out, attempt) || emitted) {
      out->error = attempt_out.error.empty() ? r.message : attempt_out.error;
      return r.ok() ? r : FsResult::error(r.code, out->error);
    }
    // exponential backoff (1000, 2000, 4000 ms by default). A flaky relay is
    // exactly the case this exists for; the wait happens on the turn thread.
    sleep_ms(policy_.backoff_ms * (1 << attempt));
  }
}

}  // namespace fs