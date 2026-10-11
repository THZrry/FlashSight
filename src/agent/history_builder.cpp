// history_builder.cpp - HistoryBuilder implementation
#include "history_builder.hpp"

#include <string>
#include <vector>

namespace fs {

int64_t HistoryBuilder::estimate_tokens(const std::string& s) {
  int64_t ascii = 0, cjk = 0;
  for (unsigned char c : s) {
    if (c >= 0x80) ++cjk;
    else ++ascii;
  }
  // 英文约 4 字符/token，CJK 约 1.5 字符/token
  return ascii / 4 + cjk * 2 / 3 + 1;
}

int64_t HistoryBuilder::estimate_tokens(const Json& j) {
  return estimate_tokens(j.dump());
}

std::string HistoryBuilder::fallback_summary(const std::string& text) {
  // 首句截断（P3 摘要模型升级留接口）
  size_t end = text.find_first_of("。\n");
  if (end == std::string::npos) end = text.size();
  std::string s = text.substr(0, end);
  if (s.size() > 200) s = s.substr(0, 200);
  return s;
}

Json HistoryBuilder::turn_to_message(const Json& turn) {
  // user / assistant 消息；tool_calls 组装
  Json msg = Json::object();
  std::string role = jsonutil::get<std::string>(turn, "role", "user");
  if (role == "user") {
    msg["role"] = "user";
    std::string text = jsonutil::get<std::string>(turn, "promptRaw", "");
    if (text.empty()) text = jsonutil::get<std::string>(turn, "streamSummary", "");
    msg["content"] = text.empty() ? "..." : text;
  } else {
    msg["role"] = "assistant";
    std::string text = jsonutil::get<std::string>(turn, "streamSummary", "");
    // 有 toolCalls 则带上（tool 结果由 P3 Loop 回填为 tool 消息）
    Json tc = turn.contains("toolCalls") && turn["toolCalls"].is_array()
                  ? turn["toolCalls"] : Json::array();
    if (!tc.empty()) {
      Json calls = Json::array();
      for (auto& c : tc) {
        if (!c.is_object()) continue;
        Json call = Json::object();
        call["id"] = jsonutil::get<std::string>(c, "id", "");
        call["type"] = "function";
        Json fn = Json::object();
        fn["name"] = jsonutil::get<std::string>(c, "name", "");
        Json args = c.contains("args") ? c["args"] : Json::object();
        fn["arguments"] = args.is_string() ? args.get<std::string>() : args.dump();
        call["function"] = fn;
        calls.push_back(call);
      }
      msg["tool_calls"] = calls;
      msg["content"] = text.empty() ? Json() : Json(text);
    } else {
      msg["content"] = text;
    }
  }
  return msg;
}

Json HistoryBuilder::build_system() const {
  Json sys = Json::object();
  sys["role"] = "system";
  std::string text = opts_.system_prefix;
  // 环境
  if (opts_.env.is_object() && !opts_.env.empty()) {
    text += "\n\n[环境]\n" + opts_.env.dump();
  }
  // 工具定义（含被禁用的系统工具说明 + skills/MCP）
  if (opts_.tools.is_array() && !opts_.tools.empty()) {
    text += "\n\n[工具定义]\n" + opts_.tools.dump();
  }
  if (opts_.mcp.is_object() && !opts_.mcp.empty()) {
    text += "\n\n[Skills 与 MCP]\n" + opts_.mcp.dump();
  }
  // 配置：只写被禁用的系统工具 + 运行期变化的自定义工具
  if (opts_.disabled_system_tools.is_array() && !opts_.disabled_system_tools.empty()) {
    text += "\n\n[禁用的系统工具]\n" + opts_.disabled_system_tools.dump();
  }
  sys["content"] = text;
  return sys;
}

HistoryResult HistoryBuilder::build(const Json& full) const {
  HistoryResult out;
  Json messages = Json::array();
  messages.push_back(build_system());
  if (!full.contains("turns") || !full["turns"].is_array()) {
    out.messages = messages;
    return out;
  }
  const Json& turns = full["turns"];
  // 短视图：最近 N 轮全量
  size_t n = static_cast<size_t>(opts_.recent_turns);
  size_t start = turns.size() > n ? turns.size() - n : 0;
  // 从 start 开始构建；跳过 forgotten
  Json msgs = Json::array();
  int64_t budget_used = 0;
  int64_t budget = opts_.token_budget;
  for (size_t i = start; i < turns.size(); ++i) {
    const Json& t = turns[i];
    if (!t.is_object()) continue;
    if (jsonutil::get<bool>(t, "forgotten", false)) continue;  // 遗忘剔除
    Json m = turn_to_message(t);
    msgs.push_back(m);
    budget_used += estimate_tokens(m);
    if (budget_used > budget) {
      // 超预算：截断（保留已加的前缀 + 最新的部分）
      // v1：超预算时把最旧的 msg 折叠为 summary（P3 摘要模型升级留接口）
      break;
    }
  }
  // 若超预算压缩：折叠最旧的消息（保留 system + 折叠条 + 最新 1 条）
  if (budget_used > budget && msgs.size() > 1) {
    out.compressed = true;
    out.compressed_from = 0;
    out.compressed_to = static_cast<int64_t>(msgs.size() - 2);
    // 用折叠 summary 替代被裁掉的消息
    Json summarized = Json::object();
    summarized["role"] = "user";
    summarized["content"] = "[早期对话已压缩] " +
                            fallback_summary(jsonutil::get<std::string>(
                                msgs[msgs.size() - 2], "content", ""));
    Json kept = Json::array();
    kept.push_back(messages[0]);  // system
    kept.push_back(summarized);
    for (size_t i = msgs.size() - 1; i < msgs.size(); ++i) {
      kept.push_back(msgs[i]);
    }
    out.messages = kept;
    return out;
  }
  for (auto& m : msgs) messages.push_back(m);
  out.messages = messages;
  return out;
}

}  // namespace fs