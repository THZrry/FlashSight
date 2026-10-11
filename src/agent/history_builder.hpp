// history_builder.hpp - HistoryBuilder (session-history.md 3, agent-loop 3)
// 从 full.json 构建运行时 history（provider 消息数组）。
#pragma once

#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {

// 运行时 history 构建结果
struct HistoryResult {
  Json messages = Json::array();   // provider 消息数组（给 ProviderClient）
  bool compressed = false;         // 本轮是否发生压缩
  int64_t compressed_from = 0;     // 压缩区间（turn 下标）
  int64_t compressed_to = 0;
};

// 提示词前缀装配（session-history.md 3 定稿顺序）：
// 前缀说明 -> 环境 -> 工具定义 -> skills与MCP -> 对话历史 -> 配置 -> 新一轮对话
class HistoryBuilder {
 public:
  struct Options {
    int recent_turns = 20;        // 短视图 N（最近 N 轮全量）
    int64_t token_budget = 8000;  // token 预算（粗略估算）
    std::string system_prefix;    // 前缀说明（如 agent 行为提示）
    Json env = Json::object();    // 环境信息
    Json tools = Json::array();   // 工具定义
    Json mcp = Json::object();    // skills 与 MCP
    Json disabled_system_tools = Json::array();  // 被禁用的系统工具
  };

  explicit HistoryBuilder(Options opts) : opts_(std::move(opts)) {}

  // 从 full.json 构建 history（含压缩决策）
  HistoryResult build(const Json& full) const;

  // 估算 token 数（粗略：英文 4 字符/token，中文 1.5 字符/token）
  static int64_t estimate_tokens(const std::string& s);
  static int64_t estimate_tokens(const Json& j);

 private:
  Options opts_;

  // 把 turn 转成 provider 消息（user/assistant）
  static Json turn_to_message(const Json& turn);
  // 生成折叠 summary（首句截断，P3 摘要模型升级留接口）
  static std::string fallback_summary(const std::string& text);
  // 装配前缀
  Json build_system() const;
};

}  // namespace fs