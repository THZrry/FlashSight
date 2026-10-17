// usage_store.hpp - UsageStore: JSONL append + aggregation (usage.md)
#pragma once

#include <map>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"

namespace fs {

struct UsageFilter {
  std::string provider;  // 空 = 不筛
  std::string model;
  int64_t from = 0;     // 0 = 不限
  int64_t to = 0;       // 0 = 不限
  std::string provider_model;  // 以 "provider:model" 保留，聚合按 provider+model 两组分开
};

struct UsageRow {
  int64_t ts = 0;
  std::string provider;
  std::string model;
  int64_t in = 0, out = 0, cache = 0;
  double cost = 0;
  std::string cost_kind;  // "money"|"quota"|"unknown"
  int64_t latency_ms = 0;
  int64_t tool_calls = 0;
  // 聚合时按天/按模型计的行数。token 为 0（供应商没回报 usage）时，调用次数
  // 仍然是真实的，图表要用它，否则整张图全是 0 高度。
  int64_t calls = 0;
  std::string error;
};

// 聚合结果（供图表）
struct UsageSummary {
  int64_t total_in = 0, total_out = 0, total_cache = 0;
  double total_cost = 0;
  std::string cost_kind;  // 依占比最高的列
  // provider×model 聚合阶梯（key = "provider::model"）
  std::map<std::string, UsageRow> by_provider_model;
  // time 桶（按天 ts/86400000）
  std::map<int64_t, UsageRow> by_day;
  uint64_t calls = 0;
  uint64_t bad_lines = 0;
};

class UsageStore {
 public:
  explicit UsageStore(const ghc::filesystem::path& calls_file)
      : calls_file_(calls_file) {}

  // 记一行（宽字段，缺省容忍）
  FsResult record(const UsageRow& row);
  FsResult record(const Json& row);  // 直接 JSON 行（宽松）

  // 聚合
  UsageSummary query(const UsageFilter& filter) const;

  // 导出 CSV 到指定文件
  FsResult export_csv(const UsageFilter& filter, const ghc::filesystem::path& out_path) const;

  // 90 天清理（usage.md §3；保留天数可配）
  FsResult purge_older_than_days(int days);

 private:
  ghc::filesystem::path calls_file_;
};

// 将 UsageRow 转成宽松 JSON 行（有值才写）
Json usage_row_to_json(const UsageRow& r);
UsageRow usage_row_from_json(const Json& j);

}  // namespace fs