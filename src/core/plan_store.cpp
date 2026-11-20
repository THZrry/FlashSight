// plan_store.cpp - the plan DOCUMENT (.flashsight/plans/<sid>.md)
#include "plan_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs {

namespace {

// The one line the loop reads. Kept as an HTML comment so it is invisible when
// the document is rendered as markdown, and human-readable in the file itself:
//   <!-- flashsight-plan status:active -->
const char kHeaderPrefix[] = "<!-- flashsight-plan status:";

std::string trim_copy(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                   s[e - 1] == '\n')) {
    --e;
  }
  return s.substr(b, e - b);
}

std::string lower_ascii(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
  }
  return s;
}

// "done"/"active" from the model's own words, normalised.
std::string norm_status(const std::string& raw) {
  std::string s = lower_ascii(trim_copy(raw));
  if (s.empty()) return "";
  if (s == "done" || s == "complete" || s == "completed" || s == "finished" ||
      s == "完成" || s == "已完成" || s == "结束") {
    return "done";
  }
  if (s == "active" || s == "running" || s == "open" || s == "进行中" ||
      s == "未完成") {
    return "active";
  }
  return s;
}

// A plan is a task document, not a file dump. 64 KB is far more than a real plan
// and keeps plan_get from flooding the model's context in one tool result.
const size_t kMaxPlanBytes = 64u * 1024u;

std::string header_field(const std::string& text, const std::string& key) {
  size_t pos = text.find(kHeaderPrefix);
  if (pos == std::string::npos) return "";
  size_t start = pos + sizeof(kHeaderPrefix) - 1;
  size_t end = text.find("-->", start);
  if (end == std::string::npos) return "";
  std::string hdr = text.substr(start, end - start);
  std::string needle = key + ":";
  size_t at = hdr.find(needle);
  if (at == std::string::npos) return "";
  at += needle.size();
  size_t stop = hdr.find(' ', at);
  return hdr.substr(at, stop == std::string::npos ? std::string::npos : stop - at);
}

int plan_seq_of(const ghc::filesystem::path& p, const std::string& text) {
  std::string s = header_field(text, "seq");
  if (!s.empty()) return std::atoi(s.c_str());
  // Fall back to plan-012.md.
  std::string stem = p.stem().string();
  if (stem.compare(0, 5, "plan-") == 0) return std::atoi(stem.c_str() + 5);
  return 1;
}

std::string plan_header(const std::string& status, int seq,
                        const std::string& turn_id) {
  std::string h = std::string(kHeaderPrefix) + status + " seq:" +
                  std::to_string(seq);
  if (!turn_id.empty()) h += " turn:" + turn_id;
  h += " -->\n";
  return h;
}

}  // namespace

std::string PlanStore::parse_status(const std::string& text) {
  size_t pos = text.find(kHeaderPrefix);
  if (pos != std::string::npos) {
    size_t start = pos + sizeof(kHeaderPrefix) - 1;
    size_t end = text.find("-->", start);
    if (end != std::string::npos) {
      std::string hdr = text.substr(start, end - start);
      size_t sp = hdr.find(' ');
      return norm_status(sp == std::string::npos ? hdr : hdr.substr(0, sp));
    }
  }
  // A model-written "Status: done" line near the top also counts: the tool says
  // what to write, but a hand-edited document should still be understood.
  size_t head_end = text.size() < 400 ? text.size() : 400;
  std::istringstream iss(text.substr(0, head_end));
  std::string line;
  while (std::getline(iss, line)) {
    std::string l = lower_ascii(trim_copy(line));
    if (l.compare(0, 7, "status:") == 0) return norm_status(line.substr(7));
    if (l.compare(0, 6, "状态:") == 0 || l.compare(0, 6, "状态：") == 0) {
      return norm_status(line.substr(6));
    }
  }
  return "";
}

Json PlanStore::get(const std::string& sid) const {
  Json out = Json::object();
  out["exists"] = false;
  out["status"] = "";
  out["content"] = "";
  out["path"] = "";
  out["seq"] = 0;
  out["turnId"] = "";
  // NOTE: the full version list (`plans`) used to be included in every get.
  // The model only needs the CURRENT document, and the UI needs its path; the
  // list of every historical plan path was pure context/serialization weight.
  if (!path_fn_ || sid.empty()) return out;
  ghc::filesystem::path base = path_fn_(sid);
  ghc::filesystem::path dir = base.parent_path();
  std::vector<ghc::filesystem::path> files;
  std::error_code ec;
  if (ghc::filesystem::is_directory(dir, ec)) {
    for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
      if (!e.path().has_filename()) continue;
      if (e.path().extension() != ".md") continue;
      if (e.path().filename().string().find(".tmp") != std::string::npos) continue;
      files.push_back(e.path());
    }
  }
  // Backward compatibility: the old single-plan path, if it still exists.
  if (files.empty() && ghc::filesystem::is_regular_file(base, ec)) {
    files.push_back(base);
  }
  if (files.empty()) {
    out["path"] = base.string();
    return out;
  }
  std::sort(files.begin(), files.end());
  ghc::filesystem::path current = files.back();
  int best_seq = -1;
  for (auto& p : files) {
    std::ifstream in(p.string(), std::ios::binary);
    if (!in) continue;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();
    int seq = plan_seq_of(p, body);
    if (seq >= best_seq) {
      best_seq = seq;
      current = p;
    }
  }
  std::ifstream in(current.string(), std::ios::binary);
  if (!in) {
    out["path"] = current.string();
    return out;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  out["exists"] = true;
  std::string status = parse_status(text);
  out["status"] = status.empty() ? "active" : status;
  out["seq"] = plan_seq_of(current, text);
  out["turnId"] = header_field(text, "turn");
  out["path"] = current.string();
  // Strip the machine header for the human/model body.
  size_t hp = text.find(kHeaderPrefix);
  if (hp != std::string::npos) {
    size_t he = text.find("-->", hp);
    if (he != std::string::npos) {
      he += 3;
      while (he < text.size() && (text[he] == '\r' || text[he] == '\n')) ++he;
      text = text.substr(0, hp) + text.substr(he);
    }
  }
  out["content"] = text;
  std::error_code tec;
  int64_t ms = 0;
  auto ft = ghc::filesystem::last_write_time(current, tec);
  if (!tec) {
    ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ft.time_since_epoch()).count());
  }
  out["updatedAt"] = ms;
  return out;
}

Json PlanStore::list(const std::string& sid) const {
  Json arr = Json::array();
  if (!path_fn_ || sid.empty()) return arr;
  ghc::filesystem::path base = path_fn_(sid);
  ghc::filesystem::path dir = base.parent_path();
  std::vector<ghc::filesystem::path> files;
  std::error_code ec;
  if (ghc::filesystem::is_directory(dir, ec)) {
    for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
      if (!e.path().has_filename() || e.path().extension() != ".md") continue;
      if (e.path().filename().string().find(".tmp") != std::string::npos) continue;
      files.push_back(e.path());
    }
  }
  if (files.empty() && ghc::filesystem::is_regular_file(base, ec)) files.push_back(base);
  std::vector<std::pair<int, ghc::filesystem::path> > seqs;
  for (auto& p : files) {
    std::ifstream in(p.string(), std::ios::binary);
    if (!in) continue;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();
    seqs.push_back(std::make_pair(plan_seq_of(p, body), p));
  }
  std::sort(seqs.begin(), seqs.end(),
            [](const std::pair<int, ghc::filesystem::path>& a,
               const std::pair<int, ghc::filesystem::path>& b) {
              return a.first < b.first;
            });
  for (auto& sp : seqs) {
    std::ifstream in(sp.second.string(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();
    Json j = Json::object();
    j["seq"] = sp.first;
    j["path"] = sp.second.string();
    std::string st = parse_status(body);
    j["status"] = st.empty() ? "active" : st;
    j["turnId"] = header_field(body, "turn");
    arr.push_back(j);
  }
  return arr;
}

FsResult PlanStore::update(const std::string& sid, const std::string& content,
                           const std::string& status, const std::string& turn_id,
                           bool new_plan) {
  if (!path_fn_ || sid.empty()) {
    return FsResult::error(ErrCode::kNotSupported, "plan store has no path");
  }
  if (content.size() > kMaxPlanBytes) {
    return FsResult::error(ErrCode::kInvalidArg,
                           "plan document is too large (max 64 KB)");
  }
  Json cur = get(sid);
  std::string prev = cur.contains("content") && cur["content"].is_string()
                         ? cur["content"].get<std::string>()
                         : std::string();
  std::string cur_status = cur.value("status", "");
  int cur_seq = static_cast<int>(jsonutil::get_int(cur, "seq", 0));
  // A plan marked done followed by a NEW body is a new task plan: keep the old
  // document and start the next numbered one. Status-only edits stay on the
  // current plan.
  bool create_new = new_plan || cur_seq <= 0 ||
                    (!content.empty() && cur_status == "done");
  int seq = create_new ? cur_seq + 1 : cur_seq;
  ghc::filesystem::path base = path_fn_(sid);
  ghc::filesystem::path p;
  if (!create_new && cur.contains("path") && cur["path"].is_string() &&
      !cur["path"].get<std::string>().empty()) {
    p = cur["path"].get<std::string>();
  } else {
    char name[32];
    std::snprintf(name, sizeof(name), "plan-%03d.md", seq);
    p = base.parent_path() / name;
  }
  std::string st = norm_status(status);
  if (st.empty()) {
    st = parse_status(prev);
    if (st.empty()) st = "active";
  }
  std::string body = content;
  if (body.empty()) body = prev;
  // Drop an existing header: the store owns exactly one, at the top.
  size_t hp = body.find(kHeaderPrefix);
  if (hp != std::string::npos) {
    size_t he = body.find("-->", hp);
    if (he != std::string::npos) {
      he += 3;
      while (he < body.size() && (body[he] == '\r' || body[he] == '\n')) ++he;
      body = body.substr(0, hp) + body.substr(he);
    }
  }
  std::string hdr = plan_header(st, seq, turn_id);
  std::string text;
  if (!body.empty() && body.compare(0, 3, "---") == 0) {
    // Keep YAML front matter at the very top; the marker follows it so tools
    // that parse front matter still see it first.
    size_t fm = body.find("\n---", 3);
    if (fm != std::string::npos) {
      size_t nl = body.find('\n', fm + 4);
      size_t at = (nl == std::string::npos) ? body.size() : nl + 1;
      text = body.substr(0, at) + hdr + body.substr(at);
    }
  }
  if (text.empty()) text = hdr + body;

  std::error_code ec;
  ghc::filesystem::create_directories(p.parent_path(), ec);
  ghc::filesystem::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
    if (!out) return FsResult::error(ErrCode::kIo, "cannot write plan document");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) return FsResult::error(ErrCode::kIo, "cannot write plan document");
  }
  ghc::filesystem::rename(tmp, p, ec);
  if (ec) {
    // Windows: rename over an existing file can fail; replace explicitly.
    ghc::filesystem::remove(p, ec);
    ec.clear();
    ghc::filesystem::rename(tmp, p, ec);
    if (ec) return FsResult::error(ErrCode::kIo, "cannot replace plan document");
  }
  // A write does not need to echo the whole document back. The model just wrote
  // it, and the UI only uses exists/status/path/seq. Returning get() duplicated
  // the entire plan into the tool result and the plan.update event.
  Json info = Json::object();
  info["exists"] = true;
  info["status"] = st;
  info["path"] = p.string();
  info["seq"] = seq;
  info["turnId"] = turn_id;
  return FsResult::success(info);
}

bool PlanStore::del(const std::string& sid) {
  reviews_.erase(sid);
  if (!path_fn_ || sid.empty()) return false;
  ghc::filesystem::path base = path_fn_(sid);
  ghc::filesystem::path dir = base.parent_path();
  bool removed = false;
  std::error_code ec;
  if (ghc::filesystem::is_directory(dir, ec)) {
    std::vector<ghc::filesystem::path> files;
    for (auto& e : ghc::filesystem::directory_iterator(dir, ec)) {
      if (e.path().extension() == ".md") files.push_back(e.path());
    }
    for (auto& p : files) removed = ghc::filesystem::remove(p, ec) || removed;
  }
  removed = ghc::filesystem::remove(base, ec) || removed;
  return removed;
}

Json PlanStore::review(const std::string& sid, const std::string& verdict,
                       const std::string& comment, int64_t now_ms) {
  Json r = Json::object();
  r["verdict"] = verdict;
  r["comment"] = comment;
  r["at"] = now_ms;
  reviews_[sid] = r;
  Json plan = get(sid);
  if (!plan.is_object()) plan = Json::object();
  plan["review"] = r;
  // reject: the document is removed so the next attempt starts from a fresh
  // plan; approve keeps it running; revise leaves it for the model to adjust
  // with the comment as guidance.
  if (verdict == "reject") del(sid);
  plan["exists"] = verdict != "reject";
  return plan;
}

}  // namespace fs
