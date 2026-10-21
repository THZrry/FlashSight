// regex_tools.cpp - file.regex 工具（addon2.md：regex 通配替换）
// 正则引擎移植自 QuickJS（WhaleJS src/libregexp.c），MIT 许可，Fabrice Bellard。
// 三个宿主回调（libregexp.c 在非 TEST 编译下需外部提供）在此实现。
#include "regex_tools.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_path.hpp"
#include "fs_simhash.hpp"  // 未用，保持 include 习惯统一

// libregexp is C. The upstream header has no extern "C" guard (it is meant to be
// compiled as part of QuickJS), so without this the C++ compiler mangles the
// names and every lre_* call fails to link.
extern "C" {
#include "libregexp.h"
}

namespace fs {

// ---- libregexp 宿主回调（QuickJS 移植要求，见 libregexp.h 末尾）----
extern "C" {

bool lre_check_stack_overflow(void* opaque, size_t alloca_size) {
  (void)opaque;
  (void)alloca_size;
  return false;  // 单线程工具调用，栈充足
}

int lre_check_timeout(void* opaque) {
  (void)opaque;
  return 0;  // 不设超时（工具级有整体超时）
}

void* lre_realloc(void* opaque, void* ptr, size_t size) {
  (void)opaque;
  return std::realloc(ptr, size);
}

}  // extern "C"

namespace {

// 二进制守卫（与 file_tools.cpp 一致）
bool is_binary(const std::string& content) {
  size_t n = content.size() < 4096 ? content.size() : 4096;
  for (size_t i = 0; i < n; ++i) {
    if (content[i] == '\0') return true;
  }
  return false;
}

bool read_text_file(const ghc::filesystem::path& p, std::string* out) {
  if (!ghc::filesystem::exists(p)) return false;
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out->resize(static_cast<size_t>(sz));
  size_t rd = std::fread(&(*out)[0], 1, static_cast<size_t>(sz), f);
  std::fclose(f);
  out->resize(rd);
  return true;
}

bool write_text_file(const ghc::filesystem::path& p, const std::string& content) {
  if (!p.parent_path().empty() && !ghc::filesystem::exists(p.parent_path())) {
    ghc::filesystem::create_directories(p.parent_path());
  }
  FILE* f = std::fopen(p.string().c_str(), "wb");
  if (!f) return false;
  size_t w = std::fwrite(content.data(), 1, content.size(), f);
  std::fclose(f);
  return w == content.size();
}

// 规范化项目内路径（防 .. 逃逸，同 file_tools.cpp）
ghc::filesystem::path resolve_in(const std::string& cwd, const std::string& rel) {
  ghc::filesystem::path base = pathutil::normalize(cwd);
  ghc::filesystem::path p = pathutil::normalize(base / rel);
  if (!pathutil::is_within(p, base) && p != base) {
    return ghc::filesystem::path();
  }
  return p;
}

// 解析 flags 字符串 -> LRE_FLAG_* 位
int parse_flags(const std::string& flags) {
  int f = 0;
  for (char c : flags) {
    switch (c) {
      case 'g': f |= LRE_FLAG_GLOBAL; break;
      case 'i': f |= LRE_FLAG_IGNORECASE; break;
      case 'm': f |= LRE_FLAG_MULTILINE; break;
      case 's': f |= LRE_FLAG_DOTALL; break;
      case 'u': f |= LRE_FLAG_UNICODE; break;
      case 'y': f |= LRE_FLAG_STICKY; break;
      default: break;  // 未知 flag 忽略
    }
  }
  return f;
}

// 替换模板展开：$& 全匹配，$1..$9 分组，$$ 字面 $（JS 风格）
std::string expand_template(const std::string& tmpl, const std::vector<std::string>& groups) {
  std::string out;
  for (size_t i = 0; i < tmpl.size(); ++i) {
    char c = tmpl[i];
    if (c == '$' && i + 1 < tmpl.size()) {
      char n = tmpl[i + 1];
      if (n == '$') { out += '$'; ++i; continue; }
      if (n == '&') { out += groups[0]; ++i; continue; }
      if (n >= '1' && n <= '9') {
        int idx = n - '0';
        if (idx < (int)groups.size() && !groups[idx].empty()) out += groups[idx];
        ++i;
        continue;
      }
    }
    out += c;
  }
  return out;
}

// 对单个文件做一次替换；返回 {changed, count, before, after}；失败返回错误
FsResult regex_file(const ghc::filesystem::path& p, const std::string& pattern,
                    const std::string& repl, int flags, int* count_out) {
  std::string content;
  if (!read_text_file(p, &content)) {
    return FsResult::error(ErrCode::kNotFound, "file not found: " + p.filename().string());
  }
  if (is_binary(content)) {
    return FsResult::error(ErrCode::kInvalidArg, "binary file: " + p.filename().string());
  }
  char err[128];
  int blen = 0;
  uint8_t* bc = lre_compile(&blen, err, sizeof(err), pattern.c_str(), pattern.size(),
                            flags, nullptr);
  if (!bc) {
    return FsResult::error(ErrCode::kInvalidArg, std::string("regex compile error: ") + err);
  }
  int cap_count = lre_get_capture_count(bc);
  int alloc_count = lre_get_alloc_count(bc);
  std::vector<uint8_t*> capture(static_cast<size_t>(alloc_count), nullptr);
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(content.data());
  std::string out;
  int count = 0;
  int pos = 0;
  size_t last_end = 0;
  int clen = static_cast<int>(content.size());
  while (pos <= clen) {
    int ret = lre_exec(capture.data(), bc, buf, pos, clen, 0, nullptr);
    if (ret != 1) break;
    int mstart = static_cast<int>(capture[0] - buf);
    int mend = static_cast<int>(capture[1] - buf);
    if (mend < mstart) { int t = mstart; mstart = mend; mend = t; }
    if (mend <= mstart && mend <= pos) {
      // 空匹配且无进展：推进 1 字节避免死循环
      if (mstart >= static_cast<int>(last_end)) {
        out.append(content, last_end,
                   static_cast<size_t>(mstart - static_cast<int>(last_end)));
      }
      if (mstart < clen) {
        out += content[static_cast<size_t>(mstart)];
        last_end = static_cast<size_t>(mstart + 1);
        pos = mstart + 1;
      } else {
        last_end = content.size();
        pos = clen + 1;
      }
      continue;
    }
    if (mstart >= static_cast<int>(last_end)) {
      out.append(content, last_end,
                 static_cast<size_t>(mstart - static_cast<int>(last_end)));
    }
    std::vector<std::string> groups;
    // capture_count 含 group 0（整体匹配）：有效下标 0..cap_count-1
    for (int g = 0; g < cap_count; ++g) {
      uint8_t* s = capture[static_cast<size_t>(g) * 2];
      uint8_t* e = capture[static_cast<size_t>(g) * 2 + 1];
      if (s && e && e >= s) {
        groups.push_back(std::string(reinterpret_cast<char*>(s), static_cast<size_t>(e - s)));
      } else {
        groups.push_back("");
      }
    }
    out += expand_template(repl, groups);
    last_end = static_cast<size_t>(mend);
    pos = mend;
    ++count;
  }
  out.append(content, last_end, content.size() - last_end);
  std::free(bc);
  if (count_out) *count_out = count;
  if (count == 0) {
    return FsResult::success(Json{{"changed", false}, {"count", 0}});
  }
  if (!write_text_file(p, out)) {
    return FsResult::error(ErrCode::kIo, "write failed: " + p.filename().string());
  }
  Json data = Json::object();
  data["changed"] = true;
  data["count"] = count;
  data["before"] = content;
  data["after"] = out;
  return FsResult::success(data);
}

}  // namespace

void register_regex_tools(ToolRegistry* registry) {
  // file.regex：单/多文件 regex 通配替换（JS 风格；替换前留 before/after 可回滚）
  ToolDef def;
  def.name = "file.regex";
  def.description =
      "regex 通配替换. Regex search-and-replace across files (JS syntax). "
      "替换模板用 $&(全匹配)/$1..$9(分组)/$$(字面$). "
      "先做替换预览? 不: 直接改并返回 before/after 供回滚. "
      "Replace directly, returns before/after for rollback.";
  Json params = Json::object();
  params["type"] = "object";
  Json props = Json::object();
  props["files"] = Json{{"type", "array"},
                        {"description", "项目内相对路径列表（单个可只给一个元素的数组）"}};
  props["pattern"] = Json{{"type", "string"}, {"description", "JS 风格正则表达式"}};
  props["replacement"] = Json{{"type", "string"}, {"description", "替换模板（$&/$1..$9/$$）"}};
  props["flags"] = Json{{"type", "string"}, {"description", "g/i/m/s/u/y（默认g）"}};
  params["properties"] = props;
  params["required"] = Json::array({"files", "pattern", "replacement"});
  def.parameters = params;
  registry->register_tool("file.regex", def,
    [](const ToolContext& ctx, const Json& args) -> FsResult {
      Json files = args.value("files", Json::array());
      if (!files.is_array() || files.empty()) {
        return FsResult::error(ErrCode::kInvalidArg, "files must be a non-empty array");
      }
      std::string pattern = jsonutil::get<std::string>(args, "pattern", "");
      std::string repl = jsonutil::get<std::string>(args, "replacement", "");
      std::string flags = jsonutil::get<std::string>(args, "flags", "g");
      if (pattern.empty()) {
        return FsResult::error(ErrCode::kInvalidArg, "empty pattern");
      }
      int f = parse_flags(flags);
      Json results = Json::array();
      int total = 0;
      for (auto& fi : files) {
        std::string rel = fi.is_string() ? fi.get<std::string>() : "";
        if (rel.empty()) continue;
        ghc::filesystem::path p = resolve_in(ctx.cwd, rel);
        if (p.empty()) {
          results.push_back(Json{{"path", rel}, {"ok", false}, {"error", "path escapes project"}});
          continue;
        }
        int cnt = 0;
        FsResult r = regex_file(p, pattern, repl, f, &cnt);
        total += cnt;
        if (r.ok()) {
          Json j = Json{{"path", rel}, {"ok", true}};
          if (r.data.contains("changed")) j["changed"] = r.data["changed"];
          j["count"] = cnt;
          if (r.data.contains("before")) j["before"] = r.data["before"];
          if (r.data.contains("after")) j["after"] = r.data["after"];
          results.push_back(j);
        } else {
          results.push_back(Json{{"path", rel}, {"ok", false}, {"error", r.message}});
        }
      }
      return FsResult::success(Json{{"results", results}, {"totalReplacements", total}});
    });
}

}  // namespace fs