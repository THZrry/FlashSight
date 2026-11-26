// plan_manager.hpp - coding-plan subscriptions (OAuth connections + quotas).
//
// A "plan" is a subscription (Claude Pro/Max, ChatGPT Plus/Codex, Gemini Code
// Assist, Z.ai GLM Coding, QwenCoding/Volc Ark/Tencent plans). It is NOT the
// pay-as-you-go API connection: each uses its own OAuth/poll flow and its own
// quota endpoint. Connections are stored separately from providers.json and are
// mirrored into a real ProviderConn when they become usable.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <ghc/filesystem.hpp>

#include "fs_clock.hpp"
#include "fs_error.hpp"
#include "fs_json.hpp"
#include "fs_net.hpp"

namespace fs {

class PlanManager {
 public:
  // Called when a connection becomes usable or its tokens are refreshed. The
  // FsApi owner mirrors it into providers.json/model table.
  using ReadyFn = std::function<void(const Json& connection)>;

  PlanManager(const ghc::filesystem::path& root, NetClient* net,
              const Clock* clock);
  ~PlanManager();

  void set_ready_fn(ReadyFn fn) { ready_ = std::move(fn); }
  void set_usage_done_fn(std::function<void()> fn) { usage_done_ = std::move(fn); }

  // Provider catalog + saved connections (tokens masked).
  Json list() const;
  // Start an OAuth/poll/key flow. `remote` is true when the UI is NOT on a
  // loopback address: remote clients cannot complete a loopback redirect.
  FsResult start(const std::string& provider, bool remote);
  // Poll a flow (auth-code callback result / GLM poll). Returns status.
  FsResult poll(const std::string& flow_id);
  // Manual code pasted by the user (Claude shows the code on its page).
  FsResult complete(const std::string& flow_id, const std::string& code,
                    const std::string& state);
  FsResult cancel(const std::string& flow_id);
  FsResult disconnect(const std::string& connection_id);
  // API-key plans (Volc/Qwen/Tencent and a manual GLM key).
  FsResult add_key(const std::string& provider, const std::string& key,
                   const std::string& region, bool remote);
  // Refresh quotas for all connections (or one) and persist lastUsage.
  FsResult refresh_usage(const std::string& connection_id);
  // True when provider_id is a connection created by this manager. FsApi calls
  // this before a model request to refresh an expired access token in place.
  bool touch_provider(const std::string& provider_id, std::string* api_key);
  // If a cached 5h/week quota is exhausted, the epoch-ms instant when the
  // account becomes usable again + 5 minutes (0 when none is exhausted).
  int64_t resume_at_ms() const;

 private:
  struct Flow;
  struct Conn;
  FsResult load();
  FsResult save();
  void done_flow(const std::string& flow_id, const Json& connection,
                 const std::string& error);
  FsResult finish_code(Flow* f, const std::string& code,
                       const std::string& state);
  FsResult exchange_tokens(Flow* f, const std::string& code);
  FsResult poll_glm(Flow* f);
  FsResult refresh_conn(Conn* c);
  Json conn_public(const Conn& c) const;
  Json conn_private(const Conn& c) const;
  Json refresh_usage_one(Conn* c);

  ghc::filesystem::path file_;
  NetClient* net_ = nullptr;
  const Clock* clock_ = nullptr;
  mutable std::recursive_mutex mu_;
  Json doc_ = Json::object();
  std::map<std::string, std::shared_ptr<Flow> > flows_;
  ReadyFn ready_;
  std::function<void()> usage_done_;
  int seq_ = 0;
};

}  // namespace fs
