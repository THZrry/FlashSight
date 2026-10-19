// usage_store.cpp - UsageStore implementation
#include "usage_store.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>

#include "fs_json.hpp"
#include "fs_jsonl.hpp"

namespace fs {

Json usage_row_to_json(const UsageRow& r) {
  Json j = Json::object();
  j["ts"] = r.ts;
  if (!r.sid.empty()) j["sid"] = r.sid;
  if (!r.provider.empty()) j["provider"] = r.provider;
  if (!r.model.empty()) j["model"] = r.model;
  if (r.in) j["in"] = r.in;
  if (r.out) j["out"] = r.out;
  if (r.cache) j["cache"] = r.cache;
  if (r.cost) j["cost"] = r.cost;
  j["cost_kind"] = r.cost_kind.empty() ? "unknown" : r.cost_kind;
  if (r.latency_ms) j["latency"] = r.latency_ms;
  if (r.tool_calls) j["toolCalls"] = r.tool_calls;
  if (!r.error.empty()) j["error"] = r.error;
  return j;
}

UsageRow usage_row_from_json(const Json& j) {
  UsageRow r;
  r.ts = jsonutil::get_int(j, "ts", 0);
  r.sid = jsonutil::get<std::string>(j, "sid", "");
  r.provider = jsonutil::get<std::string>(j, "provider", "");
  r.model = jsonutil::get<std::string>(j, "model", "");
  r.in = jsonutil::get_int(j, "in", 0);
  r.out = jsonutil::get_int(j, "out", 0);
  r.cache = jsonutil::get_int(j, "cache", 0);
  r.cost = jsonutil::get_number(j, "cost", 0.0);
  r.cost_kind = jsonutil::get<std::string>(j, "cost_kind", "unknown");
  r.latency_ms = jsonutil::get_int(j, "latency", 0);
  r.tool_calls = jsonutil::get_int(j, "toolCalls", 0);
  r.error = jsonutil::get<std::string>(j, "error", "");
  return r;
}

FsResult UsageStore::record(const UsageRow& row) {
  return record(usage_row_to_json(row));
}

FsResult UsageStore::record(const Json& row) {
  std::string err;
  JsonlWriter w(calls_file_.string());
  if (!w.append(row, &err)) return FsResult::error(ErrCode::kIo, err);
  return FsResult::success();
}

UsageSummary UsageStore::query(const UsageFilter& filter) const {
  UsageSummary sum;
  std::vector<Json> lines;
  uint64_t bad = 0;
  read_jsonl(calls_file_.string(), &lines, &bad);
  sum.bad_lines = bad;
  for (auto& j : lines) {
    UsageRow r = usage_row_from_json(j);
    if (!filter.provider.empty() && r.provider != filter.provider) continue;
    if (!filter.model.empty() && r.model != filter.model) continue;
    if (!filter.sid.empty() && r.sid != filter.sid) continue;
    if (filter.from && r.ts < filter.from) continue;
    if (filter.to && r.ts > filter.to) continue;
    ++sum.calls;
    if (!sum.has_last || r.ts >= sum.last.ts) {
      sum.last = r;
      sum.has_last = true;
    }
    sum.total_in += r.in;
    sum.total_out += r.out;
    sum.total_cache += r.cache;
    sum.total_cost += r.cost;
    if (sum.cost_kind.empty()) sum.cost_kind = r.cost_kind;
    else if (r.cost_kind == "money") sum.cost_kind = "money";  // money 优先
    std::string key = r.provider + "::" + r.model;
    UsageRow& e = sum.by_provider_model[key];
    e.ts = std::max(e.ts, r.ts);
    e.provider = r.provider;
    e.model = r.model;
    e.in += r.in; e.out += r.out; e.cache += r.cache;
    e.cost += r.cost;
    e.calls += 1;
    if (e.cost_kind.empty()) e.cost_kind = r.cost_kind;
    int64_t day = r.ts / 86400000;
    UsageRow& d = sum.by_day[day];
    d.in += r.in; d.out += r.out; d.cache += r.cache;
    d.cost += r.cost;
    d.calls += 1;
    if (d.cost_kind.empty()) d.cost_kind = r.cost_kind;
    d.ts = r.ts;
  }
  return sum;
}

namespace {
std::string csv_escape(std::string s) {
  bool need = false;
  for (char c : s) {
    if (c == '"' || c == ',' || c == '\n' || c == '\r') { need = true; break; }
  }
  std::string out;
  if (need) {
    out.push_back('"');
    for (char c : s) {
      if (c == '"') out += "\"\"";
      else out.push_back(c);
    }
    out.push_back('"');
  } else {
    out = s;
  }
  return out;
}
}  // namespace

FsResult UsageStore::export_csv(const UsageFilter& filter,
                                const ghc::filesystem::path& out_path) const {
  std::vector<Json> lines;
  read_jsonl(calls_file_.string(), &lines, nullptr);
  std::string csv = "provider,model,ts,in,out,cache,cost,cost_kind,latency,toolCalls,error\n";
  for (auto& j : lines) {
    UsageRow r = usage_row_from_json(j);
    if (!filter.provider.empty() && r.provider != filter.provider) continue;
    if (!filter.model.empty() && r.model != filter.model) continue;
    if (!filter.sid.empty() && r.sid != filter.sid) continue;
    if (filter.from && r.ts < filter.from) continue;
    if (filter.to && r.ts > filter.to) continue;
    csv += csv_escape(r.provider) + "," + csv_escape(r.model) + "," +
           std::to_string(r.ts) + "," + std::to_string(r.in) + "," +
           std::to_string(r.out) + "," + std::to_string(r.cache) + "," +
           std::to_string(r.cost) + "," + csv_escape(r.cost_kind) + "," +
           std::to_string(r.latency_ms) + "," + std::to_string(r.tool_calls) + "," +
           csv_escape(r.error) + "\n";
  }
  ghc::filesystem::path parent = out_path.parent_path();
  if (!parent.empty() && !ghc::filesystem::exists(parent)) {
    ghc::filesystem::create_directories(parent);
  }
  std::ofstream ofs(out_path.string(), std::ios::binary | std::ios::trunc);
  if (!ofs) return FsResult::error(ErrCode::kIo, "cannot open csv: " + out_path.string());
  ofs << csv;
  ofs.flush();
  if (!ofs) return FsResult::error(ErrCode::kIo, "csv write failed: " + out_path.string());
  return FsResult::success();
}

FsResult UsageStore::purge_older_than_days(int days) {
  if (days <= 0) return FsResult::success();
  if (!ghc::filesystem::exists(calls_file_)) return FsResult::success();
  int64_t now = static_cast<int64_t>(std::time(nullptr)) * 1000;
  int64_t cutoff = now - static_cast<int64_t>(days) * 86400000;
  std::vector<Json> lines;
  read_jsonl(calls_file_.string(), &lines, nullptr);
  std::string err;
  ghc::filesystem::path tmp = ghc::filesystem::path(calls_file_.string() + ".tmp");
  JsonlWriter w(tmp.string());
  bool ok = true;
  for (auto& j : lines) {
    int64_t ts = jsonutil::get_int(j, "ts", 0);
    if (ts > 0 && ts < cutoff) continue;  // 太旧删除
    if (!w.append(j, &err)) { ok = false; break; }
  }
  w.close();
  if (!ok) {
    ghc::filesystem::remove(tmp);
    return FsResult::error(ErrCode::kIo, "purge rewrite failed: " + err);
  }
  std::error_code ec;
  ghc::filesystem::rename(tmp, calls_file_, ec);
  if (ec) {
    ghc::filesystem::remove(tmp);
    return FsResult::error(ErrCode::kIo, "purge rename failed: " + ec.message());
  }
  return FsResult::success();
}

}  // namespace fs