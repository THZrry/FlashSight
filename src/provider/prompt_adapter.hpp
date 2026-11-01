// prompt_adapter.hpp - neutral conversation format -> provider wire format
//
// WHY THIS EXISTS
// The agent loop must not know which provider it is talking to. Before this
// file, the loop built Anthropic-shaped tool results ({type:"tool_result"} in a
// user message) and sent them to EVERY provider: OpenAI-compatible endpoints
// either rejected the request or silently ignored the result, so the model saw
// its own tool call with no answer and re-issued it forever.
//
// The neutral format below is also what gets persisted into full.json, which
// means a reloaded session can be replayed into a valid request for any
// provider (see HistoryBuilder).
//
//   {role:"system",    content:"..."}
//   {role:"user",      content:"..."}
//   {role:"assistant", content:"...", toolCalls:[{id, name, args}]}
//   {role:"tool",      toolCallId:"...", name:"...", content:"...", isError:false}
//
// `args` is always a JSON OBJECT in the neutral form (never a string); the
// adapter stringifies it only where the wire format demands it.
#pragma once

#include <string>

#include "fs_json.hpp"
#include "provider_types.hpp"

namespace fs {
namespace prompt {

// Neutral tool definition: {name, description, parameters}
// `parameters` is a JSON Schema object.

struct Adapted {
  Json messages = Json::array();  // provider-ready (system included for OpenAI)
  Json system;                    // Anthropic top-level system (string), null otherwise
  Json tools = Json::array();     // provider-ready tool definitions
};

// Convert one neutral conversation + neutral tool list into the wire shape of
// `kind`. Unknown kinds fall back to the OpenAI Chat shape.
Adapted adapt(ApiKind kind, const Json& neutral_messages, const Json& neutral_tools);

// Tool definitions only (used by tests and by the request builder).
Json adapt_tools(ApiKind kind, const Json& neutral_tools);

// Normalise a model-emitted tool call into {id, name, args(object)}.
// Accepts both the flat neutral form and OpenAI's {function:{name,arguments}}.
// A missing id gets `fallback_id` so the result can always be matched back.
Json normalize_tool_call(const Json& call, const std::string& fallback_id);

// True when the neutral messages already satisfy this provider's structural
// rules (alternating roles for Anthropic, tool results grouped per call, ...).
// Used by tests; adapt() repairs the common violations itself.
bool validate(ApiKind kind, const Json& neutral_messages, std::string* why);

// ---------------------------------------------------------------------------
// Tool-name wire mapping
// ---------------------------------------------------------------------------
// Both OpenAI and Anthropic require tool names to match
// `^[a-zA-Z0-9_-]{1,64}$`. This project names its tools with dots
// ("read_file", "plan_update") because that groups them by family, and a dot
// makes a request body invalid - the endpoint answers 400 before the model
// ever sees it. The wire name therefore replaces every illegal character with
// '_', and a per-request map translates the model's calls back.
//
// The mapping is per request (not global) because two different neutral names
// could collide after sanitising; the builder guarantees uniqueness inside one
// request, which is all that matters for matching a call to its tool.
struct ToolNameMap {
  // neutral name -> wire name
  std::map<std::string, std::string> to_wire;
  // wire name -> neutral name
  std::map<std::string, std::string> from_wire;

  // Register one name, disambiguating with a numeric suffix on collision.
  std::string add(const std::string& neutral);
  // Translate a model-emitted tool name back. Unknown names pass through (a
  // model may hallucinate one; the loop reports it as an unknown tool).
  std::string to_neutral(const std::string& wire) const;
};

// The wire-safe form of one tool name (no collision handling; use ToolNameMap
// for a whole request).
std::string wire_tool_name(const std::string& neutral);

}  // namespace prompt
}  // namespace fs
