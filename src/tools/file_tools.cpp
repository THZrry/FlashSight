// file_tools.cpp - file.read/ls/find/write/diffWrite/rm implementation
#include "file_tools.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <ghc/filesystem.hpp>

#include "fs_path.hpp"
#include "noise_detect.hpp"
#include "storage/fs_patch.hpp"

namespace fs {

namespace {

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

// 规范化项目内路径（防止 .. 逃逸）
ghc::filesystem::path resolve_in(const std::string& cwd, const std::string& rel) {
  ghc::filesystem::path base = pathutil::normalize(cwd);
  ghc::filesystem::path p = pathutil::normalize(base / rel);
  // 逃逸检查
  if (!pathutil::is_within(p, base) && p != base) {
    return ghc::filesystem::path();
  }
  return p;
}

// full 权限下解析路径：允许项目外（绝对或相对），但仍拒绝 .. 逃逸到无法规范化处
ghc::filesystem::path resolve_any(const std::string& cwd, const std::string& rel) {
  ghc::filesystem::path raw(rel);
  if (raw.is_absolute()) return pathutil::normalize(raw);
  return pathutil::normalize(pathutil::normalize(cwd) / rel);
}

// 按权限档解析路径：full -> 项目内外皆可；其余 -> 仅项目内
ghc::filesystem::path resolve_by_perm(const ToolContext& ctx, const std::string& rel) {
  if (ctx.permission == "full") {
    ghc::filesystem::path p = resolve_any(ctx.cwd, rel);
    return p;
  }
  return resolve_in(ctx.cwd, rel);
}

// 写权限门（addon2 A4 + permissions.md）：approve 档需人工批准（由 Loop 权限门拦），
// 这里做兜底：非 full 权限时拒绝绝对路径/项目外路径；full 允许项目外。
// 另外所有档位都过黑白名单（黑名单 > 白名单 > 三档默认）。
FsResult check_write_perm(const ToolContext& ctx, const ghc::filesystem::path& p,
                          const std::string& rel) {
  if (ctx.deny_check) {
    std::string reason;
    if (!ctx.deny_check(p.string(), &reason)) {
      return FsResult::deny("deny-list", reason.empty() ? "blocked by deny list" : reason);
    }
  }
  if (ctx.permission == "full") return FsResult::success();
  ghc::filesystem::path raw(rel);
  if (raw.is_absolute()) {
    return FsResult::deny("approve", "absolute path requires full permission");
  }
  (void)p;
  return FsResult::success();
}

// 写入噪声检测（addon2）：命中则拒绝写盘并提示模型重检查
FsResult check_noise(const std::string& content) {
  NoiseReport rep = detect_noise(content);
  if (!rep.noisy) return FsResult::success();
  FsResult r = FsResult::error(ErrCode::kInvalidArg, rep.reason);
  r.data = Json{{"noise", true}, {"suspiciousChars", rep.suspicious_chars}};
  return r;
}

}  // namespace

void register_file_tools(ToolRegistry* registry) {
  // file.read
  {
    ToolDef def;
    def.name = "file.read";
    def.description = "读取文件（offset/limit 分页；二进制文件拒绝并提示）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    props["offset"] = Json{{"type", "integer"}, {"description", "起始行（1-based，默认1）"}};
    props["limit"] = Json{{"type", "integer"}, {"description", "最大行数（默认2000）"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("file.read", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        int offset = static_cast<int>(jsonutil::get_int(args, "offset", 1));
        int limit = static_cast<int>(jsonutil::get_int(args, "limit", 2000));
        ghc::filesystem::path p = resolve_in(ctx.cwd, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        if (ctx.deny_check) {
          std::string reason;
          if (!ctx.deny_check(p.string(), &reason)) {
            return FsResult::deny("deny-list",
                                  reason.empty() ? "blocked by deny list" : reason);
          }
        }
        std::string content;
        if (!read_text_file(p, &content)) {
          return FsResult::error(ErrCode::kNotFound, "file not found: " + rel);
        }
        if (is_binary(content)) {
          return FsResult::error(ErrCode::kInvalidArg,
                                 "binary file, use vision.readImage for images: " + rel);
        }
        // 按行分页
        std::vector<std::string> lines;
        std::string cur;
        for (char c : content) {
          if (c == '\n') { lines.push_back(cur); cur.clear(); }
          else if (c != '\r') { cur.push_back(c); }
        }
        lines.push_back(cur);
        int total = static_cast<int>(lines.size());
        int start = std::max(0, offset - 1);
        int end = std::min(total, start + limit);
        std::string out;
        for (int i = start; i < end; ++i) out += lines[i] + "\n";
        Json data = Json::object();
        data["content"] = out;
        data["totalLines"] = total;
        data["startLine"] = start + 1;
        data["truncated"] = end < total;
        return FsResult::success(data);
      });
  }

  // file.ls
  // addon2（用户 2026-10-12）：基础/详细两种模式。
  // 基础（默认）：只返回文件名列表，省 token；详细：带类型/日期/大小，CSV 多行。
  {
    ToolDef def;
    def.name = "file.ls";
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
    registry->register_tool("file.ls", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", ".");
        bool detail = jsonutil::get<bool>(args, "detail", false);
        ghc::filesystem::path p = resolve_in(ctx.cwd, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        std::error_code ec;
        if (!ghc::filesystem::exists(p, ec) || !ghc::filesystem::is_directory(p, ec)) {
          return FsResult::error(ErrCode::kNotFound, "dir not found: " + rel);
        }
        Json arr = Json::array();
        // 基础：每行一个文件名
        std::string names;
        // 详细：name,type,date,size CSV
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
          // 日期（epoch 秒，UI/模型自行格式化；避免依赖系统 locale）
          int64_t mtime = 0;
          try {
            mtime = static_cast<int64_t>(ftime.time_since_epoch().count());
          } catch (...) {}
          j["mtime"] = mtime;
          arr.push_back(j);
          if (detail) {
            // CSV: 名称含逗号时用引号包裹（RFC 4180 简化版）
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
        data["entries"] = arr;               // 结构化（兼容旧接口）
        data["detail"] = detail;
        if (detail) {
          data["entriesCsv"] = csv;          // name,type,date,size
        } else {
          data["names"] = names;             // 基础：仅文件名
        }
        return FsResult::success(data);
      });
  }

  // file.find（文件名匹配 + 内容 grep）
  {
    ToolDef def;
    def.name = "file.find";
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
    registry->register_tool("file.find", def,
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
          // 跳过 .flashsight 与 .git
          if (rel.find(".flashsight") == 0 || rel.find(".git") == 0) continue;
          if (!grep) {
            if (it->path().filename().string().find(pattern) != std::string::npos) {
              arr.push_back(rel);
            }
          } else {
            if (ghc::filesystem::is_directory(it->path(), ec)) continue;
            std::string content;
            if (!read_text_file(it->path(), &content)) continue;
            if (is_binary(content)) continue;
            // 逐行找
            std::string cur;
            int lineno = 0;
            for (char c : content + "\n") {
              if (c == '\n') {
                ++lineno;
                if (cur.find(pattern) != std::string::npos) {
                  arr.push_back(Json{{"path", rel}, {"line", lineno}, {"text", cur.substr(0, 200)}});
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

  // file.write（整文件写；补 patch 记录由 FsApi 层负责）
  {
    ToolDef def;
    def.name = "file.write";
    def.description = "整文件写入（高风险；approve 门；覆盖）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    props["content"] = Json{{"type", "string"}, {"description", "完整内容"}};
    params["properties"] = props;
    params["required"] = Json::array({"path", "content"});
    def.parameters = params;
    registry->register_tool("file.write", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        std::string content = jsonutil::get<std::string>(args, "content", "");
        FsResult perm = check_write_perm(ctx, ghc::filesystem::path(rel), rel);
        if (!perm.ok()) return perm;
        ghc::filesystem::path p = resolve_by_perm(ctx, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        FsResult noise = check_noise(content);
        if (!noise.ok()) return noise;
        std::string before;
        read_text_file(p, &before);  // 不存在则 before 空
        if (!write_text_file(p, content)) {
          return FsResult::error(ErrCode::kIo, "write failed: " + rel);
        }
        Json data = Json::object();
        data["path"] = rel;
        data["before"] = before;
        data["after"] = content;
        return FsResult::success(data);
      });
  }

  // file.diffWrite（dtl 上下文匹配；对不上直接失败）
  {
    ToolDef def;
    def.name = "file.diffWrite";
    def.description = "上下文匹配写入（oldBlock 必须精确匹配一次；对不上失败）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    props["oldBlock"] = Json{{"type", "string"}, {"description", "待替换的原文块"}};
    props["newBlock"] = Json{{"type", "string"}, {"description", "替换后的新块"}};
    params["properties"] = props;
    params["required"] = Json::array({"path", "oldBlock", "newBlock"});
    def.parameters = params;
    registry->register_tool("file.diffWrite", def,
      [](const ToolContext& ctx, const Json& args) -> FsResult {
        std::string rel = jsonutil::get<std::string>(args, "path", "");
        std::string oldb = jsonutil::get<std::string>(args, "oldBlock", "");
        std::string newb = jsonutil::get<std::string>(args, "newBlock", "");
        FsResult perm = check_write_perm(ctx, ghc::filesystem::path(rel), rel);
        if (!perm.ok()) return perm;
        ghc::filesystem::path p = resolve_by_perm(ctx, rel);
        if (p.empty()) return FsResult::error(ErrCode::kInvalidArg, "path escapes project: " + rel);
        FsResult noise = check_noise(newb);
        if (!noise.ok()) return noise;
        std::string content;
        if (!read_text_file(p, &content)) {
          return FsResult::error(ErrCode::kNotFound, "file not found: " + rel);
        }
        if (is_binary(content)) {
          return FsResult::error(ErrCode::kInvalidArg, "binary file: " + rel);
        }
        // 精确匹配一次
        size_t first = content.find(oldb);
        if (first == std::string::npos) {
          return FsResult::error(ErrCode::kInvalidArg, "oldBlock not found: " + rel);
        }
        if (content.find(oldb, first + 1) != std::string::npos) {
          return FsResult::error(ErrCode::kInvalidArg, "oldBlock matches multiple times: " + rel);
        }
        std::string after = content.substr(0, first) + newb + content.substr(first + oldb.size());
        if (!write_text_file(p, after)) {
          return FsResult::error(ErrCode::kIo, "write failed: " + rel);
        }
        Json data = Json::object();
        data["path"] = rel;
        data["before"] = content;
        data["after"] = after;
        return FsResult::success(data);
      });
  }

  // file.rm（直接删除；确认走批准门；删前快照由 FsApi 层留 patch）
  {
    ToolDef def;
    def.name = "file.rm";
    def.description = "直接删除文件/空目录（高风险；approve 门；不进回收站）";
    Json params = Json::object();
    params["type"] = "object";
    Json props = Json::object();
    props["path"] = Json{{"type", "string"}, {"description", "项目内相对路径"}};
    params["properties"] = props;
    params["required"] = Json::array({"path"});
    def.parameters = params;
    registry->register_tool("file.rm", def,
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