// agent_context.hpp - builds the model-facing context (system prefix + per-turn tail)
//
// WHY A SEPARATE MODULE
// The context is the product. Two constraints shape it:
//
//  1. PREFIX CACHE. Providers bill cached input tokens at a fraction of the
//     price, but only while the prefix is byte-identical. So everything stable
//     (identity, capabilities, environment, project instructions, memory) is
//     emitted first, in a fixed order, and everything volatile (time, plan,
//     todos, subagent status) is appended AFTER the conversation, as part of
//     the newest user message. A volatile field placed in the prefix would
//     invalidate the cache on every single turn.
//
//  2. TOKEN BUDGET. Tool schemas are already sent through the API's `tools`
//     parameter; repeating them here would double the cost of the largest
//     fixed block. The prefix therefore carries only a one-line index of what
//     each tool is for - the "when to use it" knowledge that a JSON schema
//     cannot express.
//
// The layout is the one fixed in doc-internal/session-history.md §3:
//   prefix(identity -> environment -> tools -> skills/MCP -> project -> memory)
//   -> conversation -> config(runtime) -> new user turn
#pragma once

#include <string>

#include "fs_json.hpp"

namespace fs {

enum class ContextLang { kZh, kEn };

// Everything the builder needs. Plain data: the agent layer must not reach into
// settings/session stores, so AgentHost fills this in from its own sources.
struct ContextInput {
  // --- identity / mode ---
  std::string agent_name = "FlashSight";
  // Parent session id. For a subagent this is the session its events are
  // attributed to (the subagent itself has no session of its own).
  std::string sid;
  // Project id: scopes per-project settings (search endpoint, CLI tools, ...).
  std::string pid;
  std::string role = "main";        // "main" | "subagent"
  std::string subagent_kind;        // preset name for subagents
  std::string subagent_task;        // the task the subagent was spawned with
  // The checklist item this subagent implements ("" = not step-scoped). A step
  // subagent must not re-plan the whole task: it owns exactly one item.
  std::string subagent_step;
  bool auto_mode = false;
  std::string permission = "permissive";  // approve | permissive | full
  std::string model;
  std::string thinking;             // level; "" = disabled
  std::string thinking_lang;        // "zh" | "en" | "" (no explicit request)
  // The language the ANSWER should be written in ("zh" | "en" | "" = let the
  // model decide). Distinct from `thinking_lang`: one is the language of the
  // hidden reasoning, the other of the answer the user reads.
  std::string output_lang;
  ContextLang lang = ContextLang::kEn;

  // --- environment ---
  std::string cwd;
  std::string os;                   // windows | linux | macos
  std::string shell;
  std::string encoding;
  Json cli_tools = Json::object();   // {name: path}  (settings > environment > cliTools)
  Json env_names = Json::array();    // variable NAMES only; values may be secrets
  Json git = Json::object();         // {branch, dirty, ahead, behind, operation}
  std::string session_title;

  // --- capabilities ---
  Json tools = Json::array();        // neutral defs [{name, description, parameters}]
  Json skills = Json::array();       // [{name, description, loadable, enabled}]
  Json mcp = Json::array();          // [{name, enabled}]
  Json disabled_tools = Json::array();
  bool allow_plan = true;
  bool allow_subagents = true;
  bool allow_workflow = true;
  bool allow_ask = true;
  int max_subagent_parallel = 4;
  int max_subagent_depth = 3;

  // --- project / memory ---
  std::string project_instructions;  // AGENTS.md / .flashsight/instructions.md
  std::string instructions_source;   // file it came from (for citation)
  Json memory = Json::object();      // long-term user preferences
};

// Volatile per-turn state. Rendered as a block INSIDE the newest user message,
// after the cached conversation, so it never invalidates the prefix.
struct RuntimeState {
  int64_t now_ms = 0;
  Json plan = Json::object();        // {goal, steps:[{title,status}]}
  Json todos = Json::array();        // [{id, task, done}]
  Json subagents = Json::array();    // [{id, kind, status, summary}]
  Json pending_wakes = Json::array();
  int64_t context_tokens = 0;        // last request's prompt tokens (water level)
  int64_t context_limit = 0;
  std::string turn_note;             // e.g. "resumed after interruption"
};

class AgentContext {
 public:
  // Stable prefix. Deterministic for identical input: no timestamps, no
  // iteration over unordered containers, no random ids.
  static std::string build_system(const ContextInput& in);

  // Volatile tail, prepended to the new user message.
  static std::string build_runtime(const ContextInput& in, const RuntimeState& st);

  // One-line tool index used in the prefix.
  static std::string tool_index(const ContextInput& in);

  // Planning / subagent / workflow guidance. Split out so tests can assert the
  // section exists and mentions every mechanism the product relies on.
  static std::string work_guidance(const ContextInput& in);

  // The <context> wrapper around the newest user input.
  static std::string wrap_user_turn(const std::string& runtime, const std::string& text);

  // Parse the wrapper back out (used by tests and by history replay).
  static std::string unwrap_user_text(const std::string& content);
};

}  // namespace fs
