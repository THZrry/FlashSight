// history_builder.hpp - full.json -> neutral conversation (session-history.md 3)
//
// The output is PROVIDER-NEUTRAL. prompt::adapt() turns it into the wire format
// of whichever endpoint the session uses. Keeping the neutral form as the
// intermediate (instead of building OpenAI/Anthropic shapes directly) is what
// lets one history replay into any provider and lets the same transcript be
// re-sent after the user switches models mid-session.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {

// 运行时 history 构建结果
struct HistoryResult {
  Json messages = Json::array();   // 中性消息数组（prompt::adapt 之前）
  bool compressed = false;         // 本轮是否发生压缩
  int64_t compressed_from = 0;     // 压缩区间（turn 下标）
  int64_t compressed_to = 0;
  int64_t dropped_turns = 0;       // 被折叠的 turn 数
  int64_t est_tokens = 0;          // 估算的对话 token（不含 system）
  // Estimated size of the NEXT request after compaction: system + tool prefix +
  // the summary/short view. The UI uses this for the water level; `est_tokens`
  // stays content-only so compression can be compared with the full replay.
  int64_t projected_tokens = 0;
  // How history was compacted this turn:
  //   "full"       - threshold/forced: EVERY stored turn folded into a summary
  //   "short-view" - the current window cannot hold the prompt (e.g. the user
  //                  switched to a smaller model): keep the newest turns and
  //                  fold the rest. The UI must tell the user this happened.
  std::string mode;
  // The fold that was produced (mode=="full") or reused. Persisting it lets the
  // next run start from the summary instead of replaying every stored turn.
  std::string summary;
  int64_t compacted_turns = 0;     // leading turns represented by `summary`
  std::string compacted_turn_id;   // turnId of the last compacted turn
};

// 提示词前缀装配（session-history.md 3 定稿顺序）：
// 前缀说明 -> 环境 -> 工具定义 -> skills与MCP -> 对话历史 -> 配置 -> 新一轮对话
class HistoryBuilder {
 public:
  struct Options {
    std::string system_prefix;    // 前缀说明（AgentContext::build_system 产出）
    Json env = Json::object();    // 环境信息（旧字段，保留兼容）
    Json tools = Json::array();   // 工具定义（旧字段，保留兼容）
    Json mcp = Json::object();    // skills 与 MCP（旧字段，保留兼容）
    Json disabled_system_tools = Json::array();  // 被禁用的系统工具
    // 上一次压缩留下的摘要（compaction.json）。没有就把被折叠轮的要点重新拼一遍。
    std::string prior_summary;
    // How many leading turns `prior_summary` already represents. 0 = none.
    // `prior_turn_id` guards against a fork/rollback that changed the turns.
    int64_t compacted_turns = 0;
    std::string prior_turn_id;
    // How the persisted summary was produced: "full" or "short-view". A short
    // view is only reused while the window is still too small; with a bigger
    // window the full transcript can be replayed again.
    std::string prior_mode;
    // 被折叠区间最多保留多少条要点，防止摘要自己撑爆预算。
    int max_fold_points = 40;
    // Messages to replay BEFORE the current turn, for a run that continues an
    // earlier conversation (a subagent follow-up). They are placed after the
    // system prompt and before the newest user message, exactly like stored
    // history - so the model sees its own previous turns and their tool results.
    Json seed_messages = Json::array();

    // ----- 水位驱动压缩 -----
    // 上一次请求的 prompt tokens（0 = 未知/首轮）。这是压缩的**主触发条件**：
    // 水位达到 compress_at_pct 就主动压缩，不等撑爆预算。
    int64_t last_prompt_tokens = 0;
    // 模型上下文窗口（0 = 未知）。未知时水位触发不可用，只有显式"立即压缩"。
    int64_t context_limit = 0;
    // 水位阈值（百分比）。到达后**全量压缩**：所有轮折成摘要，不保留任何轮的
    // 原文，也不截断消息。短视图只是窗口装不下时把摘要进一步压小的 fallback
    // （例如换到小窗模型），仍然覆盖全部历史。<=0 关闭水位触发。
    int compress_at_pct = 80;
    // Deprecated: compaction no longer keeps a suffix of raw turns. The field is
    // kept for source compatibility with older callers and is ignored.
    int keep_turns = 0;
    // Model-backed summariser: takes the whole session material (prior summary
    // + every user requirement + assistant answer + tool outcome) and returns a
    // model-written summary. Compaction must preserve meaning, not spend a
    // hard-coded byte budget per turn. Empty callback = deterministic fallback
    // (unit tests / embedded callers).
    std::function<std::string(const std::string& material)> model_summarizer;
    // 强制压缩（用户点"压缩"/UI 提示时）。
    bool force_compress = false;
  };

  explicit HistoryBuilder(Options opts) : opts_(std::move(opts)) {}

  // 从 full.json 构建中性消息（含压缩决策）
  HistoryResult build_neutral(const Json& full) const;

  // 压缩是否该触发（水位 / 预算 / 强制）。暴露给 UI 与单测：
  // "现在该压缩了"是一个可观测的判断，不应该只藏在 build 里。
  struct CompressDecision {
    bool compress = false;
    std::string reason;      // "water-level" | "token-budget" | "forced" | ""
    int64_t prompt_tokens = 0;
    int64_t context_limit = 0;
    int pct = 0;             // 水位百分比（窗口未知时 0）
  };
  static CompressDecision should_compress(const Options& opts, int64_t turns_tokens);

  // 兼容旧调用：OpenAI Chat 形态（system 在 messages 里）
  HistoryResult build(const Json& full) const;

  // 兼容旧调用：Anthropic 形态（{system, messages:[{role, content:[blocks]}]}）
  HistoryResult build_anthropic(const Json& full) const;

  // 估算 token 数（粗略：英文 4 字符/token，CJK 1 字符/token）
  static int64_t estimate_tokens(const std::string& s);
  static int64_t estimate_tokens(const Json& j);

  // 把一组 turn 压成一段可读摘要（首句 + 要点）。供压缩轮与 runtime 缓存使用。
  static std::string fold_summary(const Json& turns, size_t from, size_t to,
                                  int max_points);

  // What a tool result looks like when it is REPLAYED into the next request.
  //
  // The stored `detail` is the whole result, pretty-printed and capped at 4000
  // characters - it exists so the user can read what the tool did. Sending that
  // back verbatim on every later turn is what made a long session burn its
  // context: a read_file body, a list_dir with 800 entries, an edit's before/
  // after text, all re-sent forever.
  //
  // The model only needs what lets it CONTINUE: did it work, which file, which
  // lines, what hash to edit with, and a bounded sample of the payload. That is
  // the projection below - per tool, because "what matters" differs (an edit's
  // before/after text is bulk, a terminal's TAIL is the signal).
  //
  // `result` is the structured result (compact JSON); `detail` is the display
  // text. Either may be empty; the projection falls back to the other.
  static std::string project_tool_result(const std::string& name,
                                         const Json& result,
                                         const std::string& detail);

 private:
  Options opts_;

  // 单个 turn -> 若干中性消息（user 轮可能带 tool 结果）
  static void turn_to_neutral(const Json& turn, Json* out);
  // 生成折叠摘要（含 keyPoints）
  static std::string make_fold(const Json& turns, size_t from, size_t to,
                               const std::string& prior, int max_points,
                               size_t max_digest_bytes = 32000);
  // 装配前缀（legacy：直接用 system_prefix + env/tools/mcp 拼接）
  Json build_system() const;
};

}  // namespace fs
