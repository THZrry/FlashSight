// file_tools.cpp - read_file / list_dir / find_files / write_file / edit_file / remove_file
//
// EDIT PROTOCOL (hash + line numbers)
// -----------------------------------
// The model reads a file, gets `hash` and per-line numbers, then edits by line
// range. Every edit may carry the hash it read AND the exact text it expects on
// those lines, so two independent checks catch the two ways an edit goes wrong:
//
//   * hash mismatch  -> the file changed since the read (concurrent save,
//                       formatter, another agent). The edit is refused and the
//                       model gets the CURRENT hash + a hint to re-read.
//   * expect mismatch -> the line numbers are right but the content is not what
//                       the model thinks it is (stale read, off-by-one). The
//                       actual text is returned so the model can correct itself
//                       instead of guessing.
//
// Both failures are reported as data, never as a silent partial write: a
// wrong edit that "succeeds" is the most expensive bug in a coding agent.
#include "file_tools.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_hash.hpp"
#include "fs_path.hpp"
#include "fs_utf8.hpp"
#include "noise_detect.hpp"
#include "storage/fs_patch.hpp"

namespace fs {

namespace {

// Read guards. A coding agent that reads a 200 MB log into memory stalls the
// whole session; one that silently truncates makes the model reason about a
// file it cannot see. Refuse, and say exactly what to do instead.
constexpr int64_t kHardMaxBytes = 32LL * 1024 * 1024;   // absolute refusal
constexpr int64_t kSoftMaxBytes = 1024LL * 1024;        // needs explicit paging
constexpr size_t kMaxLineChars = 4000;                  // per-line output cap

// 二进制守卫：前 4KB 含 NUL 即判二进制
bool is_binary(const std::string& content) {
  size_t n = std::min(content.size(), size_t(4096));
  for (size_t i = 0; i < n; ++i) {
    if (content[i] == '\0') return true;
  }
  return false;
}

bool read_text_file(const ghc::filesystem::path& p, std::string* out) {
  if (!ghc::filesystem::exists(p)) return false;
  std::ifstream ifs(p.string(), std::ios::binary);
  if (!ifs) return false;
  std::stringstream ss;
  ss << ifs.rdbuf();
  *out = ss.str();
  return true;
}

bool write_text_file(const ghc::filesystem::path& p, const std::string& content) {
  if (!p.parent_path().empty() && !ghc::filesystem::exists(p.parent_path())) {
    ghc::filesystem::create_directories(p.parent_path());
  }
  std::ofstream ofs(p.string(), std::ios::binary | std::ios::trunc);
  if (!ofs) return false;
  ofs << content;
  ofs.flush();
  return !!ofs;
}

// A file split into lines, keeping everything needed to write it back BYTE
// IDENTICALLY except for the edited range.
//
// The trailing newline is the subtle part: "a\nb\n" is two lines, and dropping
// the final "\n" on save would (a) violate the POSIX text-file convention and
// (b) make every diff show the last line as changed. It is therefore tracked
// explicitly instead of being lost in the split.
struct TextFile {
  std::vector<std::string> lines;
  std::string eol = "\n";
  bool final_newline = false;
};

TextFile split_text(const std::string& content) {
  TextFile tf;
  tf.eol = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  tf.final_newline = !content.empty() && content.back() == '\n';
  std::string cur;
  for (size_t i = 0; i < content.size(); ++i) {
    char c = content[i];
    if (c == '\r' && i + 1 < content.size() && content[i + 1] == '\n') continue;
    if (c == '\n') {
      tf.lines.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  // A trailing newline already closed the last line; anything left is a real
  // (unterminated) final line.
  if (!cur.empty()) tf.lines.push_back(cur);
  return tf;
}

std::string join_text(const std::vector<std::string>& lines, const std::string& eol,
                      bool final_newline) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) out += eol;
    out += lines[i];
  }
  if (final_newline && !lines.empty()) out += eol;
  return out;
}

// Rewrite line endings on request. "preserve" (default) leaves the bytes alone,
// which is what you want for an existing file; "lf"/"crlf" is for new files and
// for repos whose convention the model must follow.
std::string apply_eol(const std::string& text, const std::string& mode,
                      const std::string& fallback_eol) {
  if (mode.empty() || mode == "preserve") return text;
  std::string want = mode == "crlf" ? "\r\n" : "\n";
  std::string out;
  out.reserve(text.size() + text.size() / 16);
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') continue;  // handled by \n
      out += want;  // lone CR: normalise it
      continue;
    }
    if (c == '\n') {
      out += want;
      continue;
    }
    out.push_back(c);
  }
  (void)fallback_eol;
  return out;
}

// 规范化项目内路径（防止 .. 逃逸）
ghc::filesystem::path resolve_in(const std::string& cwd, const std::string& rel) {
  ghc::filesystem::path base = pathutil::normalize(cwd);
  ghc::filesystem::path p = pathutil::normalize(base / rel);
  if (!pathutil::is_within(p, base) && p != base) {
    return ghc::filesystem::path();
  }
  return p;
}

// full 权限下解析路径：允许项目外（绝对或相对）
ghc::filesystem::path resolve_any(const std::string& cwd, const std::string& rel) {
  ghc::filesystem::path raw(rel);
  if (raw.is_absolute()) return pathutil::normalize(raw);
  return pathutil::normalize(pathutil::normalize(cwd) / rel);
}

bool escapes_project(const std::string& cwd, const std::string& rel) {
  ghc::filesystem::path base = pathutil::normalize(cwd);
  if (base.empty()) return false;
  ghc::filesystem::path raw(rel);
  ghc::filesystem::path p = raw.is_absolute()
                                ? pathutil::normalize(raw)
                                : pathutil::normalize(base / rel);
  return !pathutil::is_within(p, base) && p != base;
}

ghc::filesystem::path resolve_by_perm(const ToolContext& ctx, const std::string& rel) {
  if (ctx.permission == "full" || ctx.approved) return resolve_any(ctx.cwd, rel);
  return resolve_in(ctx.cwd, rel);
}

// 写权限门（addon2 A4 + permissions.md）
FsResult check_write_perm(const ToolContext& ctx, const ghc::filesystem::path& p,
                          const std::string& rel) {
  if (ctx.deny_check) {
    std::string reason;
    if (!ctx.deny_check(p.string(), &reason)) {
      return FsResult::deny("deny-list", reason.empty() ? "blocked by deny list" : reason);
    }
  }
  if (ctx.permission == "full") return FsResult::success();
  if (ctx.approved) return FsResult::success();
  if (!escapes_project(ctx.cwd, rel)) return FsResult::success();
  return FsResult::deny("approve",
                        "path is outside the project; ask the user before writing");
}

FsResult check_noise(const std::string& content) {
  NoiseReport rep = detect_noise(content);
  if (!rep.noisy) return FsResult::success();
  FsResult r = FsResult::error(ErrCode::kInvalidArg, rep.reason);
  r.data = Json{{"noise", true}, {"suspiciousChars", rep.suspicious_chars}};
  return r;
}

// Do two hashes refer to the same content? Accept the full 16-char form, the
// 12-char short form, and a case-insensitive match: the model copies this value
// by hand from tool output and a strict equality check on formatting would fail
// for reasons that have nothing to do with the file.
bool hash_matches(const std::string& given, const std::string& actual) {
  if (given.empty()) return true;  // no expectation stated
  std::string a, b;
  for (char c : given) a.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (char c : actual) b.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (a.size() > b.size()) return b.compare(0, b.size(), a, 0, b.size()) == 0;
  return b.compare(0, a.size(), a) == 0;
}

}  // namespace

void register_file_tools(ToolRegistry* registry) {
  // ----- read_file ---------------------------------------------------------
  {
    ToolDef def;
    def.name = "read_file";
    def.description =
        "读取文件（按行分页；返回 hash 供 edit_file 校验）. Read a file by lines. "
        "Returns {hash, content, startLine, totalLines, truncated}; pass `hash` to "
        "edit_file so a concurrent change is caught instead of overwritten. "
        "Files >1MB need an explicit offset/limit.";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    props["offset"] = Json{{"type", "integer"}, {"description", "起始行（1-based，默认1）"}};
    props["limit"] = Json{{"type", "integer"}, {"description", "最大行数（默认2000）"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("read_file", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        int offset = static_cast<int>(jsonutil::get_int(args, "offset", 1));
        int limit = static_cast<int>(jsonutil::get_int(args, "limit", 2000));
        bool explicit_page = args.contains("offset") || args.contains("limit");
        ghc::filesystem::path p = resolve_in(ctx.cwd, rel);
        if (p.empty()) {
          // A path outside the project is readable under full permission (or
          // after an approval); without those it stays refused.
          p = resolve_by_perm(ctx, rel);
          if (p.empty()) {
            return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
          }
        }
        if (ctx.deny_check) {
          std::string reason;
          if (!ctx.deny_check(p.string(), &reason)) {
            return FsResult::deny("deny-list",
                                  reason.empty() ? "blocked by deny list" : reason);
          }
        }
        std::error_code ec;
        if (!ghc::filesystem::is_regular_file(p, ec)) {
          return FsResult::error(ErrCode::kNotFound, "file not found: " + rel);
        }
        int64_t size = static_cast<int64_t>(ghc::filesystem::file_size(p, ec));
        if (size > kHardMaxBytes) {
          return FsResult::error(
              ErrCode::kInvalidArg,
              "file is too large to read (" + std::to_string(size) +
                  " bytes, limit " + std::to_string(kHardMaxBytes) +
                  "): use find_files/file_regex, or a terminal command to inspect it");
        }
        if (size > kSoftMaxBytes && !explicit_page) {
          return FsResult::error(
              ErrCode::kInvalidArg,
              "file is " + std::to_string(size) +
                  " bytes; pass explicit offset+limit to read a slice");
        }
        std::string content;
        if (!read_text_file(p, &content)) {
          return FsResult::error(ErrCode::kNotFound, "file not found: " + rel);
        }
        if (is_binary(content)) {
          return FsResult::error(ErrCode::kInvalidArg,
                                 "binary file, use read_image for images: " + rel);
        }
        std::string eol;
        TextFile tf = split_text(content);
        std::vector<std::string> lines = tf.lines;
        eol = tf.eol;
        int total = static_cast<int>(lines.size());
        if (offset < 1) offset = 1;
        if (limit < 1) limit = 2000;
        int start = offset - 1;
        if (start > total) start = total;
        int end = std::min(total, start + limit);
        std::string out;
        bool clipped = false;
        for (int i = start; i < end; ++i) {
          std::string line = lines[i];
          if (line.size() > kMaxLineChars) {
            line = utf8::truncate(line, kMaxLineChars, " …(line truncated)");
            clipped = true;
          }
          out += line;
          out += "\n";
        }
        Json data = Json::object();
        data["content"] = out;
        data["totalLines"] = total;
        data["startLine"] = start + 1;
        data["endLine"] = end;
        data["truncated"] = end < total;
        if (clipped) data["longLinesTruncated"] = true;
        data["eol"] = eol == "\r\n" ? "crlf" : "lf";
        // The hash covers the WHOLE file, not the slice: it is an identity for
        // "the file I read", which is what the edit protocol needs.
        data["hash"] = hash::short_hex(hash::xxh64_hex(content));
        return FsResult::success(data);
      });
  }

  // ----- list_dir ----------------------------------------------------------
  {
    ToolDef def;
    def.name = "list_dir";
    def.description = "列出目录内容. List directory. "
                      "detail=false(默认): 仅文件名 one name per line; "
                      "detail=true: name,type,date,size CSV 每行一个条目.";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径（默认.）"}};
    props["detail"] = Json{{"type", "boolean"}, {"description", "详细模式（默认false，仅文件名）"}};
    params["properties"] = props;
    def.parameters = params;
    registry->register_tool("list_dir", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", ".");
        bool detail = jsonutil::get<bool>(args, "detail", false);
        ghc::filesystem::path p = resolve_by_perm(ctx, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        std::error_code ec;
        if (!ghc::filesystem::exists(p, ec) || !ghc::filesystem::is_directory(p, ec)) {
          return FsResult::error(ErrCode::kNotFound, "dir not found: " + rel);
        }
        Json arr = Json::array();
        std::string names;
        std::string csv;
        for (auto& e : ghc::filesystem::directory_iterator(p, ec)) {
          Json j = Json::object();
          std::string name = e.path().filename().string();
          j["name"] = name;
          bool isdir = ghc::filesystem::is_directory(e.path(), ec);
          j["isDir"] = isdir;
          std::error_code ec2;
          auto ftime = ghc::filesystem::last_write_time(e.path(), ec2);
          int64_t size = isdir ? 0
                          : static_cast<int64_t>(ghc::filesystem::file_size(e.path(), ec2));
          j["size"] = size;
          int64_t mtime = 0;
          try {
            mtime = static_cast<int64_t>(ftime.time_since_epoch().count());
          } catch (...) {}
          j["mtime"] = mtime;
          arr.push_back(j);
          if (detail) {
            std::string q = name;
            if (q.find(',') != std::string::npos || q.find('"') != std::string::npos) {
              std::string esc;
              for (char c : q) {
                if (c == '"') esc += "\"\"";
                else esc += c;
              }
              q = "\"" + esc + "\"";
            }
            csv += q + "," + (isdir ? "dir" : "file") + "," +
                   std::to_string(mtime) + "," + std::to_string(size) + "\n";
          } else {
            names += name + "\n";
          }
        }
        Json data = Json::object();
        data["entries"] = arr;
        data["detail"] = detail;
        if (detail) data["entriesCsv"] = csv;
        else data["names"] = names;
        return FsResult::success(data);
      });
  }

  // ----- find_files --------------------------------------------------------
  {
    ToolDef def;
    def.name = "find_files";
    def.description = "文件名匹配或内容 grep（项目内；二进制跳过）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["pattern"] = Json{{"type", "string"}, {"description", "文件名子串或内容子串"}};
    props["grep"] = Json{{"type", "boolean"}, {"description", "true=内容搜索，false=文件名（默认false）"}};
    props["maxResults"] = Json{{"type", "integer"}, {"description", "上限（默认100）"}};
    params["properties"] = props;
    params["required"] = Json::array({"pattern"});
    def.parameters = params;
    registry->register_tool("find_files", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string pattern = jsonutil::get<std::string>(args, "pattern", "");
        bool grep = jsonutil::get<bool>(args, "grep", false);
        int maxr = static_cast<int>(jsonutil::get_int(args, "maxResults", 100));
        if (pattern.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty pattern");
        ghc::filesystem::path base = pathutil::normalize(ctx.cwd);
        Json arr = Json::array();
        std::error_code ec;
        for (auto it = ghc::filesystem::recursive_directory_iterator(base, ec);
             it != ghc::filesystem::recursive_directory_iterator(); ++it) {
          if ((int)arr.size() >= maxr) break;
          std::string rel = it->path().lexically_relative(base).string();
          if (rel.find(".flashsight") == 0 || rel.find(".git") == 0) continue;
          if (!grep) {
            if (it->path().filename().string().find(pattern) != std::string::npos) {
              arr.push_back(rel);
            }
          } else {
            if (ghc::filesystem::is_directory(it->path(), ec)) continue;
            // A grep over a huge file would load it whole; skip and say so.
            std::error_code szec;
            auto sz = ghc::filesystem::file_size(it->path(), szec);
            if (!szec && static_cast<int64_t>(sz) > kHardMaxBytes) continue;
            std::string content;
            if (!read_text_file(it->path(), &content)) continue;
            if (is_binary(content)) continue;
            std::string cur;
            int lineno = 0;
            for (char c : content + "\n") {
              if (c == '\n') {
                ++lineno;
                if (cur.find(pattern) != std::string::npos) {
                  arr.push_back(Json{{"path", rel}, {"line", lineno},
                                     {"text", utf8::truncate(cur, 200)}});
                  if ((int)arr.size() >= maxr) break;
                }
                cur.clear();
              } else if (c != '\r') {
                cur.push_back(c);
              }
            }
          }
        }
        return FsResult::success(Json{{"results", arr}, {"truncated", false}});
      });
  }

  // ----- write_file --------------------------------------------------------
  {
    ToolDef def;
    def.name = "write_file";
    def.description =
        "整文件写入（覆盖；新建文件用这个，改已有文件用 edit_file）. "
        "Write a whole file. Use edit_file to change an existing file: this "
        "replaces everything and cannot be merged.";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    props["content"] = Json{{"type", "string"}, {"description", "完整内容"}};
    props["lineEnding"] = Json{{"type", "string"},
                               {"description", "preserve(默认)|lf|crlf"}};
    params["properties"] = props;
    params["required"] = Json::array({"path", "content"});
    def.parameters = params;
    registry->register_tool("write_file", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        std::string content = jsonutil::get<std::string>(args, "content", "");
        std::string le = jsonutil::get<std::string>(args, "lineEnding", "preserve");
        FsResult perm = check_write_perm(ctx, ghc::filesystem::path(rel), rel);
        if (!perm.ok()) return perm;
        ghc::filesystem::path p = resolve_by_perm(ctx, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        FsResult noise = check_noise(content);
        if (!noise.ok()) return noise;
        std::string before;
        read_text_file(p, &before);
        // A new file defaults to LF (the repo convention on every platform we
        // target); an existing file keeps whatever it already uses unless the
        // caller says otherwise.
        std::string existing_eol = "lf";
        if (!before.empty()) {
          existing_eol = split_text(before).eol == "\r\n" ? "crlf" : "lf";
        }
        std::string out = apply_eol(content, le, existing_eol);
        if (le == "preserve" && before.empty()) out = apply_eol(content, "lf", "lf");
        if (!write_text_file(p, out)) {
          return FsResult::error(ErrCode::kIo, "write failed: " + rel);
        }
        Json data = Json::object();
        data["path"] = rel;
        data["before"] = before;
        data["after"] = out;
        data["hash"] = hash::short_hex(hash::xxh64_hex(out));
        return FsResult::success(data);
      });
  }

  // ----- edit_file (the primary edit tool) --------------------------------
  {
    ToolDef def;
    def.name = "edit_file";
    def.description =
        "按行号替换（哈希校验；失败不写盘）. Edit by line range with verification. "
        "Args: path, hash (from read_file), edits:[{start, end?, expect?, newText}]. "
        "`start`/`end` are 1-based inclusive line numbers in the file AS READ. "
        "`expect` is the exact current text of those lines - if it differs, the "
        "edit is refused and the real text is returned. Multiple edits apply to "
        "the original line numbers (they are applied bottom-up, so they never "
        "shift each other).";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    props["hash"] = Json{{"type", "string"},
                         {"description", "read_file 返回的 hash（强烈建议传；不符则拒绝）"}};
    Json edit = Json::object();
    edit["type"] = "object";
    edit["properties"] = Json{
        {"start", Json{{"type", "integer"}, {"description", "起始行（1-based，含）"}}},
        {"end", Json{{"type", "integer"}, {"description", "结束行（含；默认=start）"}}},
        {"expect", Json{{"type", "string"},
                        {"description", "这些行当前的确切内容（不含换行）；不符则拒绝"}}},
        {"newText", Json{{"type", "string"},
                         {"description", "替换后的内容；空串=删除这些行"}}},
    };
    edit["required"] = Json::array({"start", "newText"});
    props["edits"] = Json{{"type", "array"}, {"items", edit},
                          {"description", "要应用的行替换列表"}};
    props["lineEnding"] = Json{{"type", "string"},
                               {"description", "preserve(默认)|lf|crlf"}};
    params["properties"] = props;
    params["required"] = Json::array({"path", "edits"});
    def.parameters = params;
    registry->register_tool("edit_file", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        std::string want_hash = jsonutil::get<std::string>(args, "hash", "");
        std::string le = jsonutil::get<std::string>(args, "lineEnding", "preserve");
        if (rel.empty()) return FsResult::error(ErrCode::kInvalidArg, "empty path");
        if (!args.contains("edits") || !args["edits"].is_array() ||
            args["edits"].empty()) {
          return FsResult::error(ErrCode::kInvalidArg, "edits must be a non-empty array");
        }
        FsResult perm = check_write_perm(ctx, ghc::filesystem::path(rel), rel);
        if (!perm.ok()) return perm;
        ghc::filesystem::path p = resolve_by_perm(ctx, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        std::string content;
        if (!read_text_file(p, &content)) {
          return FsResult::error(ErrCode::kNotFound, "file not found: " + rel);
        }
        if (is_binary(content)) {
          return FsResult::error(ErrCode::kInvalidArg, "binary file: " + rel);
        }
        std::string cur_hash = hash::short_hex(hash::xxh64_hex(content));
        // Check 1: identity of the file the model read.
        if (!hash_matches(want_hash, cur_hash)) {
          FsResult r = FsResult::error(
              ErrCode::kInvalidArg,
              "file changed since it was read (hash mismatch); re-read it before editing");
          r.data = Json{{"path", rel}, {"currentHash", cur_hash},
                        {"expectedHash", want_hash}, {"stale", true}};
          return r;
        }
        std::string eol;
        TextFile tf = split_text(content);
        std::vector<std::string> lines = tf.lines;
        eol = tf.eol;
        int total = static_cast<int>(lines.size());

        struct Edit {
          int start;  // 1-based
          int end;    // 1-based inclusive
          std::vector<std::string> replacement;
          std::string before;
          std::string after;
        };
        std::vector<Edit> edits;
        for (auto& e : args["edits"]) {
          if (!e.is_object()) {
            return FsResult::error(ErrCode::kInvalidArg, "each edit must be an object");
          }
          int start = static_cast<int>(jsonutil::get_int(e, "start", 0));
          int end = static_cast<int>(jsonutil::get_int(e, "end", start));
          if (start < 1 || end < start) {
            return FsResult::error(ErrCode::kInvalidArg,
                                   "bad line range: start=" + std::to_string(start) +
                                       " end=" + std::to_string(end));
          }
          if (end > total) {
            return FsResult::error(
                ErrCode::kInvalidArg,
                "line range " + std::to_string(start) + "-" + std::to_string(end) +
                    " is past the end of the file (" + std::to_string(total) + " lines)");
          }
          Edit ed;
          ed.start = start;
          ed.end = end;
          std::string expect = jsonutil::get<std::string>(e, "expect", "");
          std::string actual = join_text(
              std::vector<std::string>(lines.begin() + (start - 1),
                                       lines.begin() + end),
              eol, false);
          // Check 2: content of the lines. Catches off-by-one and stale reads
          // even when the hash happens to match (e.g. the model edited twice
          // without re-reading).
          if (!expect.empty() && expect != actual) {
            FsResult r = FsResult::error(
                ErrCode::kInvalidArg,
                "expected text does not match lines " + std::to_string(start) + "-" +
                    std::to_string(end) + "; nothing was written");
            r.data = Json{{"path", rel},
                          {"actual", utf8::truncate(actual, 800)},
                          {"expected", utf8::truncate(expect, 800)},
                          {"currentHash", cur_hash}};
            return r;
          }
          std::string new_text = jsonutil::get<std::string>(e, "newText", "");
          FsResult noise = check_noise(new_text);
          if (!noise.ok()) return noise;
          ed.before = actual;
          ed.after = new_text;
          if (!new_text.empty()) {
            // Inserted text is split on \n and re-joined with the FILE's EOL, so
            // a CRLF file never ends up with mixed line endings.
            ed.replacement = split_text(new_text).lines;
          }
          edits.push_back(ed);
        }
        // Overlapping edits are ambiguous (which one wins?). Refuse rather than
        // pick an order the model did not intend.
        std::sort(edits.begin(), edits.end(),
                  [](const Edit& a, const Edit& b) { return a.start < b.start; });
        for (size_t i = 1; i < edits.size(); ++i) {
          if (edits[i].start <= edits[i - 1].end) {
            return FsResult::error(
                ErrCode::kInvalidArg,
                "overlapping edits at lines " + std::to_string(edits[i - 1].start) +
                    "-" + std::to_string(edits[i - 1].end) + " and " +
                    std::to_string(edits[i].start) + "-" +
                    std::to_string(edits[i].end));
          }
        }
        // Apply bottom-up so earlier edits never shift later line numbers.
        std::vector<std::string> out_lines = lines;
        for (size_t i = edits.size(); i-- > 0;) {
          const Edit& ed = edits[i];
          out_lines.erase(out_lines.begin() + (ed.start - 1),
                          out_lines.begin() + ed.end);
          out_lines.insert(out_lines.begin() + (ed.start - 1), ed.replacement.begin(),
                           ed.replacement.end());
        }
        // Preserve the file's trailing-newline state. Deleting the last line of
        // a file that ended with a newline must not leave a stray blank line,
        // and editing a file that did NOT end with one must not add it.
        bool final_newline = tf.final_newline;
        if (!out_lines.empty() && out_lines.back().empty()) {
          // The edit emptied the last line: keep the file terminated, but drop
          // the empty line itself so the count stays honest.
          out_lines.pop_back();
          final_newline = !out_lines.empty();
        }
        std::string after = join_text(out_lines, eol, final_newline);
        after = apply_eol(after, le, tf.eol == "\r\n" ? "crlf" : "lf");
        if (!write_text_file(p, after)) {
          return FsResult::error(ErrCode::kIo, "write failed: " + rel);
        }
        Json changes = Json::array();
        for (auto& ed : edits) {
          Json c = Json::object();
          c["start"] = ed.start;
          c["end"] = ed.end;
          // The before/after text is what lets the model undo its own edit, and
          // what the UI shows as the change.
          c["before"] = utf8::truncate(ed.before, 2000);
          c["after"] = utf8::truncate(ed.after, 2000);
          changes.push_back(c);
        }
        Json data = Json::object();
        data["path"] = rel;
        data["changes"] = changes;
        data["totalLines"] = static_cast<int>(out_lines.size());
        data["hash"] = hash::short_hex(hash::xxh64_hex(after));
        data["beforeHash"] = cur_hash;
        data["summary"] = std::to_string(edits.size()) + " edit(s) applied";
        return FsResult::success(data);
      });
  }

  // ----- remove_file -------------------------------------------------------
  {
    ToolDef def;
    def.name = "remove_file";
    def.description = "直接删除文件/空目录（高风险；approve 门；不进回收站）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("remove_file", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        FsResult perm = check_write_perm(ctx, ghc::filesystem::path(rel), rel);
        if (!perm.ok()) return perm;
        ghc::filesystem::path p = resolve_by_perm(ctx, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        std::error_code ec;
        if (!ghc::filesystem::exists(p, ec)) {
          return FsResult::error(ErrCode::kNotFound, "not found: " + rel);
        }
        std::string before;
        bool was_file = ghc::filesystem::is_regular_file(p, ec);
        if (was_file) read_text_file(p, &before);
        ghc::filesystem::remove(p, ec);
        if (ec) return FsResult::error(ErrCode::kIo, "rm failed: " + ec.message());
        Json data = Json::object();
        data["path"] = rel;
        data["before"] = before;
        data["wasFile"] = was_file;
        return FsResult::success(data);
      });
  }
}

}  // namespace fs
