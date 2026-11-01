// prompt_adapter.cpp - neutral -> wire format per provider (see header)
#include "prompt_adapter.hpp"

namespace fs {
namespace prompt {

namespace {

std::string get_str(const Json& j, const char* k, const char* def = "") {
  return jsonutil::get<std::string>(j, k, def);
}

// Serialize tool arguments exactly once, at the boundary. Everything inside the
// agent keeps them as objects so they can be inspected/redacted/replayed.
std::string args_to_wire(const Json& args) {
  if (args.is_string()) return args.get<std::string>();  // already wire-shaped
  if (args.is_null()) return "{}";
  return args.dump();
}

Json text_block(const std::string& text) {
  Json b = Json::object();
  b["type"] = "text";
  b["text"] = text;
  return b;
}

// Split a data URI into mime + base64 payload ("data:image/png;base64,AAAA").
void split_data_uri(const std::string& uri, std::string* mime, std::string* b64) {
  mime->clear();
  b64->clear();
  if (uri.compare(0, 5, "data:") != 0) {
    *b64 = uri;
    return;
  }
  size_t comma = uri.find(',');
  if (comma == std::string::npos) return;
  std::string head = uri.substr(5, comma - 5);
  size_t semi = head.find(';');
  *mime = semi == std::string::npos ? head : head.substr(0, semi);
  if (mime->empty()) *mime = "image/png";
  *b64 = uri.substr(comma + 1);
}

// Does this neutral message carry image parts?
bool has_parts(const Json& m) {
  return m.contains("parts") && m["parts"].is_array() && !m["parts"].empty();
}

// OpenAI Chat: content becomes an array of {type:text} / {type:image_url}.
Json openai_parts(const Json& m) {
  Json arr = Json::array();
  for (auto& p : m["parts"]) {
    if (!p.is_object()) continue;
    std::string type = get_str(p, "type");
    if (type == "text") {
      arr.push_back(Json{{"type", "text"}, {"text", get_str(p, "text")}});
    } else if (type == "image") {
      std::string uri = get_str(p, "dataUri");
      if (uri.empty()) continue;
      Json img = Json::object();
      img["url"] = uri;
      arr.push_back(Json{{"type", "image_url"}, {"image_url", img}});
    }
  }
  return arr;
}

// Anthropic: {type:"image", source:{type:"base64", media_type, data}}.
Json anthropic_parts(const Json& m) {
  Json arr = Json::array();
  for (auto& p : m["parts"]) {
    if (!p.is_object()) continue;
    std::string type = get_str(p, "type");
    if (type == "text") {
      arr.push_back(text_block(get_str(p, "text")));
    } else if (type == "image") {
      std::string mime, b64;
      split_data_uri(get_str(p, "dataUri"), &mime, &b64);
      if (b64.empty()) continue;
      Json src = Json::object();
      src["type"] = "base64";
      src["media_type"] = mime;
      src["data"] = b64;
      Json img = Json::object();
      img["type"] = "image";
      img["source"] = src;
      arr.push_back(img);
    }
  }
  return arr;
}

// Anthropic wants alternating roles and all tool results for one assistant
// message grouped in the following user message. Merge adjacent same-role
// messages by concatenating their content blocks.
void push_merge(Json* out, Json msg) {
  if (!out->is_array()) *out = Json::array();
  if (!out->empty()) {
    Json& last = (*out)[out->size() - 1];
    if (get_str(last, "role") == get_str(msg, "role")) {
      // merge content: string + string -> "\n\n"-joined string; otherwise blocks
      Json a = last.value("content", Json(""));
      Json b = msg.value("content", Json(""));
      if (a.is_string() && b.is_string()) {
        last["content"] = a.get<std::string>() + "\n\n" + b.get<std::string>();
        return;
      }
      Json blocks = Json::array();
      auto append_blocks = [&blocks](const Json& c) {
        if (c.is_array()) {
          for (auto& x : c) blocks.push_back(x);
        } else if (c.is_string()) {
          blocks.push_back(text_block(c.get<std::string>()));
        }
      };
      append_blocks(a);
      append_blocks(b);
      last["content"] = blocks;
      return;
    }
  }
  out->push_back(msg);
}

Json openai_tool_message(const Json& m) {
  Json o = Json::object();
  o["role"] = "tool";
  o["tool_call_id"] = get_str(m, "toolCallId");
  // A tool result that carries an image (a screenshot) must send the image as
  // content parts; a JSON string containing a data URI would be text to the
  // model, i.e. it could not see it.
  if (has_parts(m)) {
    o["content"] = openai_parts(m);
  } else {
    o["content"] = get_str(m, "content");
  }
  return o;
}

Json openai_assistant_message(const Json& m) {
  Json o = Json::object();
  o["role"] = "assistant";
  std::string text = get_str(m, "content");
  Json calls = m.contains("toolCalls") && m["toolCalls"].is_array() ? m["toolCalls"]
                                                                   : Json::array();
  if (!calls.empty()) {
    Json arr = Json::array();
    for (auto& c : calls) {
      if (!c.is_object()) continue;
      Json call = Json::object();
      call["id"] = get_str(c, "id");
      call["type"] = "function";
      Json fn = Json::object();
      fn["name"] = get_str(c, "name");
      fn["arguments"] = args_to_wire(c.contains("args") ? c["args"] : Json::object());
      call["function"] = fn;
      arr.push_back(call);
    }
    o["tool_calls"] = arr;
    // OpenAI requires content to be null (not "") when the message is only a
    // tool call; an empty string is accepted by some servers and rejected by
    // others, null is the portable spelling.
    o["content"] = text.empty() ? Json() : Json(text);
  } else {
    o["content"] = text;
  }
  return o;
}

Json anthropic_user_message(const Json& m) {
  Json o = Json::object();
  o["role"] = "user";
  if (has_parts(m)) {
    o["content"] = anthropic_parts(m);
  } else {
    o["content"] = Json::array({text_block(get_str(m, "content"))});
  }
  return o;
}

Json anthropic_assistant_message(const Json& m) {
  Json blocks = Json::array();
  std::string text = get_str(m, "content");
  if (!text.empty()) blocks.push_back(text_block(text));
  Json calls = m.contains("toolCalls") && m["toolCalls"].is_array() ? m["toolCalls"]
                                                                   : Json::array();
  for (auto& c : calls) {
    if (!c.is_object()) continue;
    Json b = Json::object();
    b["type"] = "tool_use";
    b["id"] = get_str(c, "id");
    b["name"] = get_str(c, "name");
    Json args = c.contains("args") ? c["args"] : Json::object();
    b["input"] = args.is_object() ? args : Json::object();
    blocks.push_back(b);
  }
  Json o = Json::object();
  o["role"] = "assistant";
  o["content"] = blocks;
  return o;
}

Json anthropic_tool_result_message(const Json& m) {
  Json b = Json::object();
  b["type"] = "tool_result";
  b["tool_use_id"] = get_str(m, "toolCallId");
  if (has_parts(m)) {
    // Anthropic tool_result content can be an array of text/image blocks, which
    // is how a screenshot reaches the model.
    b["content"] = anthropic_parts(m);
  } else {
    b["content"] = get_str(m, "content");
  }
  if (jsonutil::get<bool>(m, "isError", false)) b["is_error"] = true;
  Json o = Json::object();
  o["role"] = "user";
  o["content"] = Json::array({b});
  return o;
}

Json response_api_item(const Json& m) {
  std::string role = get_str(m, "role");
  if (role == "tool") {
    Json o = Json::object();
    o["type"] = "function_call_output";
    o["call_id"] = get_str(m, "toolCallId");
    o["output"] = get_str(m, "content");
    return o;
  }
  Json o = Json::object();
  o["role"] = role;
  o["content"] = get_str(m, "content");
  if (role == "assistant") {
    Json calls = m.contains("toolCalls") && m["toolCalls"].is_array() ? m["toolCalls"]
                                                                     : Json::array();
    if (!calls.empty()) {
      // The Responses API represents a call as its own input item; the text
      // (if any) stays on the assistant message.
      Json arr = Json::array();
      if (!get_str(m, "content").empty()) arr.push_back(o);
      for (auto& c : calls) {
        if (!c.is_object()) continue;
        Json item = Json::object();
        item["type"] = "function_call";
        item["call_id"] = get_str(c, "id");
        item["name"] = get_str(c, "name");
        item["arguments"] = args_to_wire(c.contains("args") ? c["args"] : Json::object());
        arr.push_back(item);
      }
      return Json{{"__array", arr}};  // expanded by the caller
    }
  }
  return o;
}

}  // namespace

Json normalize_tool_call(const Json& call, const std::string& fallback_id) {
  Json out = Json::object();
  std::string id = get_str(call, "id");
  std::string name = get_str(call, "name");
  Json args = call.contains("args") ? call["args"] : Json::object();
  bool parse_error = false;
  if (name.empty() && call.contains("function") && call["function"].is_object()) {
    const Json& fn = call["function"];
    name = get_str(fn, "name");
    if (fn.contains("arguments")) {
      const Json& a = fn["arguments"];
      if (a.is_string()) {
        std::string raw = a.get<std::string>();
        if (raw.empty()) {
          args = Json::object();
        } else {
          try {
            args = Json::parse(raw);
          } catch (...) {
            // Never guess: hand the model a parse error so it can retry with
            // valid JSON instead of running the tool with empty arguments.
            args = Json::object();
            args["_raw"] = raw;
            parse_error = true;
          }
        }
      } else if (a.is_object()) {
        args = a;
      }
    }
  } else if (args.is_string()) {
    std::string raw = args.get<std::string>();
    if (raw.empty()) {
      args = Json::object();
    } else {
      try {
        args = Json::parse(raw);
      } catch (...) {
        args = Json::object();
        args["_raw"] = raw;
        parse_error = true;
      }
    }
  }
  if (args.is_null() || !args.is_object()) args = Json::object();
  out["id"] = id.empty() ? fallback_id : id;
  out["name"] = name;
  out["args"] = args;
  // Preserve a flag set by an earlier pass: run_loop normalizes once to build
  // the assistant message and run_tool_group normalizes again before executing.
  // Dropping the flag in the second pass would execute a write with the broken
  // arguments - the exact thing the flag exists to prevent.
  if (parse_error || jsonutil::get<bool>(call, "argsParseError", false)) {
    out["argsParseError"] = true;
  }
  return out;
}

Json adapt_tools(ApiKind kind, const Json& neutral_tools) {
  Json arr = Json::array();
  if (!neutral_tools.is_array()) return arr;
  for (auto& t : neutral_tools) {
    if (!t.is_object()) continue;
    std::string name = get_str(t, "name");
    if (name.empty()) continue;
    Json params = t.contains("parameters") && t["parameters"].is_object()
                      ? t["parameters"] : Json::object();
    if (params.empty()) {
      params = Json::object();
      params["type"] = "object";
      params["properties"] = Json::object();
    }
    std::string desc = get_str(t, "description");
    if (kind == ApiKind::kAnthropic) {
      Json o = Json::object();
      o["name"] = name;
      o["description"] = desc;
      o["input_schema"] = params;
      arr.push_back(o);
    } else if (kind == ApiKind::kOpenAIResponse) {
      Json o = Json::object();
      o["type"] = "function";
      o["name"] = name;
      o["description"] = desc;
      o["parameters"] = params;
      arr.push_back(o);
    } else {
      Json o = Json::object();
      o["type"] = "function";
      Json fn = Json::object();
      fn["name"] = name;
      fn["description"] = desc;
      fn["parameters"] = params;
      o["function"] = fn;
      arr.push_back(o);
    }
  }
  return arr;
}

Adapted adapt(ApiKind kind, const Json& neutral_messages, const Json& neutral_tools) {
  Adapted out;
  out.tools = adapt_tools(kind, neutral_tools);
  Json msgs = neutral_messages.is_array() ? neutral_messages : Json::array();

  if (kind == ApiKind::kAnthropic) {
    std::string system;
    Json list = Json::array();
    for (auto& m : msgs) {
      if (!m.is_object()) continue;
      std::string role = get_str(m, "role");
      if (role == "system") {
        std::string s = get_str(m, "content");
        if (!s.empty()) system += (system.empty() ? "" : "\n\n") + s;
        continue;
      }
      if (role == "tool") {
        push_merge(&list, anthropic_tool_result_message(m));
      } else if (role == "assistant") {
        push_merge(&list, anthropic_assistant_message(m));
      } else {
        push_merge(&list, anthropic_user_message(m));
      }
    }
    // Anthropic requires the first message to be from the user. A history that
    // begins with an assistant turn (compacted sessions do) would be rejected
    // outright, so prefix a neutral continuation marker instead of dropping it.
    if (!list.empty() && get_str(list[0], "role") == "assistant") {
      Json lead = anthropic_user_message(Json{{"content", "[earlier context]"}});
      Json with_lead = Json::array({lead});
      for (auto& x : list) with_lead.push_back(x);
      list = with_lead;
    }
    out.messages = list;
    out.system = system.empty() ? Json() : Json(system);
    return out;
  }

  if (kind == ApiKind::kOpenAIResponse) {
    Json list = Json::array();
    for (auto& m : msgs) {
      if (!m.is_object()) continue;
      Json item = response_api_item(m);
      if (item.is_object() && item.contains("__array")) {
        for (auto& x : item["__array"]) list.push_back(x);
      } else {
        list.push_back(item);
      }
    }
    out.messages = list;
    out.system = Json();
    return out;
  }

  // OpenAI Chat / Custom / legacy Plan: system stays inline.
  Json list = Json::array();
  for (auto& m : msgs) {
    if (!m.is_object()) continue;
    std::string role = get_str(m, "role");
    if (role == "tool") {
      list.push_back(openai_tool_message(m));
    } else if (role == "assistant") {
      list.push_back(openai_assistant_message(m));
    } else if (role == "system") {
      list.push_back(Json{{"role", "system"}, {"content", get_str(m, "content")}});
    } else if (has_parts(m)) {
      // A user message with images (a pasted screenshot): content parts.
      list.push_back(Json{{"role", "user"}, {"content", openai_parts(m)}});
    } else {
      list.push_back(Json{{"role", "user"}, {"content", get_str(m, "content")}});
    }
  }
  out.messages = list;
  out.system = Json();
  return out;
}

bool validate(ApiKind kind, const Json& neutral_messages, std::string* why) {
  auto fail = [why](const std::string& s) {
    if (why) *why = s;
    return false;
  };
  if (!neutral_messages.is_array()) return fail("messages is not an array");
  bool saw_user = false;
  std::string prev;
  for (auto& m : neutral_messages) {
    if (!m.is_object()) return fail("message is not an object");
    std::string role = get_str(m, "role");
    if (role != "system" && role != "user" && role != "assistant" && role != "tool") {
      return fail("unknown role: " + role);
    }
    if (role == "tool") {
      if (get_str(m, "toolCallId").empty()) return fail("tool message without toolCallId");
      continue;
    }
    if (role == "user") saw_user = true;
    if (kind == ApiKind::kAnthropic && role == "assistant" && !saw_user) {
      return fail("anthropic history starts with assistant");
    }
    if (kind == ApiKind::kAnthropic && role == prev && role != "system") {
      return fail("anthropic has adjacent same-role messages: " + role);
    }
    prev = role;
  }
  return true;
}

}  // namespace prompt
}  // namespace fs
