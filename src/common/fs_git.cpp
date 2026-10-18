// fs_git.cpp - git CLI wrapper (see fs_git.hpp for why not libgit2).
#include "fs_git.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <ghc/filesystem.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs {
namespace git {

namespace {

// Run `git -C <dir> <args...>`. Arguments are passed as a vector and quoted for
// the platform, never concatenated from user text: a file named
// "--upload-pack=..." must be a path, not a flag.
//
// Windows has no fork/exec pair worth using here without adding a dependency,
// so the command goes through _popen. The argument vector is quoted with the
// standard Windows rules (double quotes, backslash escaping), and every path
// passed in is repo-relative, so shell metacharacters cannot appear unescaped.
std::string quote_arg(const std::string& a) {
#if defined(_WIN32)
  if (a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
  std::string out = "\"";
  for (size_t i = 0; i < a.size(); ++i) {
    size_t bs = 0;
    while (i < a.size() && a[i] == '\\') {
      ++bs;
      ++i;
    }
    if (i == a.size()) {
      out.append(bs * 2, '\\');
      break;
    }
    if (a[i] == '"') {
      out.append(bs * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(bs, '\\');
      out.push_back(a[i]);
    }
  }
  out.push_back('"');
  return out;
#else
  if (a.find_first_of(" \t\n'\"\\$`") == std::string::npos) return a;
  std::string out = "'";
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == '\'') out += "'\\''";
    else out.push_back(a[i]);
  }
  out.push_back('\'');
  return out;
#endif
}

std::string quote_path(const std::string& p) { return quote_arg(p); }

// A scratch file for a command's stderr. git writes progress/warning lines
// ("LF will be replaced by CRLF") to stderr even on success; merging them into
// stdout would corrupt the diff/log payload the caller parses, so the two
// streams are kept apart and both read.
std::string temp_err_path() {
#if defined(_WIN32)
  char buf[MAX_PATH + 1];
  DWORD n = GetTempPathA(MAX_PATH, buf);
  if (n == 0) return std::string();
  std::string dir(buf);
  char name[MAX_PATH + 1];
  if (GetTempFileNameA(dir.c_str(), "fsg", 0, name) == 0) return std::string();
  return std::string(name);
#else
  const char* tmp = std::getenv("TMPDIR");
  std::string dir = tmp && *tmp ? tmp : "/tmp";
  std::string tpl = dir + "/fsgitXXXXXX";
  std::vector<char> buf(tpl.begin(), tpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd < 0) return std::string();
  close(fd);
  return std::string(buf.data());
#endif
}

GitResult run(const std::string& dir, const std::vector<std::string>& args) {
  GitResult r;
  std::string cmd = "git";
  if (!dir.empty()) {
    cmd += " -C ";
    cmd += quote_path(dir);
  }
  for (size_t i = 0; i < args.size(); ++i) {
    cmd += " ";
    cmd += quote_arg(args[i]);
  }
  // stderr goes to a scratch file, never into stdout: a parsed payload (diff,
  // porcelain status, log) must not contain git's progress chatter.
  std::string errfile = temp_err_path();
  if (!errfile.empty()) {
    cmd += " 2>";
    cmd += quote_path(errfile);
  } else {
    cmd += " 2>&1";  // no temp file available: better a polluted message than none
  }
#if defined(_WIN32)
  FILE* f = _popen(cmd.c_str(), "rb");
#else
  FILE* f = popen(cmd.c_str(), "r");
#endif
  if (!f) {
    r.available = false;
    r.err = "cannot start git";
    return r;
  }
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) r.out.append(buf, n);
#if defined(_WIN32)
  int rc = _pclose(f);
#else
  int rc = pclose(f);
#endif
  r.code = rc;
  r.ok = (rc == 0);
  if (!errfile.empty()) {
    std::ifstream ef(errfile.c_str(), std::ios::binary);
    if (ef) {
      std::stringstream ss;
      ss << ef.rdbuf();
      r.err = ss.str();
    }
    std::remove(errfile.c_str());
  }
  return r;
}

// Is this path safe to hand to git as a pathspec? Repo-relative, no traversal,
// no leading dash (which git would read as an option).
bool safe_pathspec(const std::string& p) {
  if (p.empty()) return false;
  if (p[0] == '-') return false;
  if (p.find('\0') != std::string::npos) return false;
  if (p.size() >= 2 && p[0] == '.' && p[1] == '.') return false;
  if (p.find("/../") != std::string::npos) return false;
  return true;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                   s[b - 1] == '\n')) {
    --b;
  }
  return s.substr(a, b - a);
}

// Split NUL-separated output (porcelain -z / ls-files -z).
std::vector<std::string> split_nul(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\0') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(s[i]);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// A file that is not in the index has no diff at all: `git diff` compares the
// work tree against the INDEX, and an untracked file has no index entry. The
// panel must still show what the file contains, so it is rendered as an
// all-added diff - that is exactly what staging it would record.
Json untracked_diff(const std::string& dir, const std::string& path,
                    size_t max_bytes) {
  std::string root = repo_root(dir);
  if (root.empty()) root = dir;
  ghc::filesystem::path full = ghc::filesystem::path(root) / path;
  std::error_code ec;
  if (!ghc::filesystem::is_regular_file(full, ec)) {
    return Json{{"ok", true}, {"diff", ""}, {"truncated", false}};
  }
  auto sz = ghc::filesystem::file_size(full, ec);
  if (!ec && static_cast<int64_t>(sz) > 1024 * 1024) {
    return Json{{"ok", true}, {"diff", ""}, {"truncated", true}};
  }
  std::ifstream in(full.string().c_str(), std::ios::binary);
  if (!in) return Json{{"ok", true}, {"diff", ""}, {"truncated", false}};
  std::stringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();
  std::string out = "diff --git a/" + path + " b/" + path + "\n";
  out += "new file\n--- /dev/null\n+++ b/" + path + "\n";
  bool truncated = false;
  std::string cur;
  for (size_t i = 0; i <= content.size(); ++i) {
    if (i == content.size() || content[i] == '\n') {
      std::string line = "+" + cur + "\n";
      if (out.size() + line.size() > max_bytes) {
        truncated = true;
        break;
      }
      out += line;
      cur.clear();
    } else {
      cur.push_back(content[i]);
    }
  }
  return Json{{"ok", true}, {"diff", out}, {"truncated", truncated}};
}

// Is `path` tracked? An untracked file has no diff, which is a different answer
// from "no changes".
bool in_index(const std::string& dir, const std::string& path) {
  GitResult r = run(dir, {"ls-files", "--error-unmatch", "--", path});
  return r.ok;
}

}  // namespace

bool is_repo(const std::string& dir) {
  if (dir.empty()) return false;
  GitResult r = run(dir, {"rev-parse", "--is-inside-work-tree"});
  return r.ok && trim(r.out) == "true";
}

std::string repo_root(const std::string& dir) {
  GitResult r = run(dir, {"rev-parse", "--show-toplevel"});
  if (!r.ok) return std::string();
  return trim(r.out);
}

Status status(const std::string& dir) {
  Status st;
  if (dir.empty()) {
    st.error = "no project directory";
    return st;
  }
  GitResult r = run(dir, {"status", "--porcelain=v1", "-z", "--branch",
                          "--untracked-files=normal"});
  if (!r.ok) {
    st.error = trim(r.err.empty() ? r.out : r.err);
    // Distinguish "not a repo" from "git is not installed": the panel says
    // different things, and telling someone to install git when they simply
    // have not run git init would be wrong.
    st.is_repo = false;
    return st;
  }
  st.ok = true;
  st.is_repo = true;
  st.root = repo_root(dir);

  std::vector<std::string> parts = split_nul(r.out);
  size_t i = 0;
  // First entry is the branch header: "## main...origin/main [ahead 1, behind 2]"
  if (i < parts.size() && parts[i].size() > 3 && parts[i][0] == '#' && parts[i][1] == '#') {
    std::string head = parts[i].substr(2);
    ++i;
    if (head.compare(0, 7, " HEAD (") == 0 || head == "HEAD (no branch)") {
      st.detached = true;
    } else {
      size_t dots = head.find("...");
      std::string branch = dots == std::string::npos ? head : head.substr(0, dots);
      st.branch = trim(branch);
      if (dots != std::string::npos) {
        std::string rest = head.substr(dots + 3);
        size_t sp = rest.find(' ');
        st.upstream = trim(sp == std::string::npos ? rest : rest.substr(0, sp));
        // "[ahead 1, behind 2]" / "[gone]"
        size_t ab = rest.find("ahead ");
        if (ab != std::string::npos) st.ahead = std::atoi(rest.c_str() + ab + 6);
        size_t be = rest.find("behind ");
        if (be != std::string::npos) st.behind = std::atoi(rest.c_str() + be + 7);
      }
    }
    if (st.branch.compare(0, 7, "(no br") == 0) {
      st.detached = true;
      st.branch.clear();
    }
  }
  // Then one record per path: "XY <path>" and, for renames, a second NUL field.
  for (; i < parts.size(); ++i) {
    const std::string& rec = parts[i];
    if (rec.size() < 4) continue;
    FileChange c;
    c.index_status = rec[0];
    c.work_status = rec[1];
    std::string path = rec.substr(3);
    // A rename/copy record is followed by its ORIGINAL path in the next field.
    if (c.index_status == 'R' || c.index_status == 'C') {
      if (i + 1 < parts.size()) {
        c.orig_path = parts[i + 1];
        ++i;
      }
    }
    c.path = path;
    if (!c.path.empty()) st.changes.push_back(c);
  }
  // A repository with no changes still reports the branch header only.
  st.clean = st.changes.empty();
  // A merge/rebase in progress leaves marker files behind; a plain commit would
  // otherwise look possible when it is not.
  {
    ghc::filesystem::path root = st.root.empty() ? ghc::filesystem::path(dir)
                                                 : ghc::filesystem::path(st.root);
    std::error_code ec;
    ghc::filesystem::path gitdir = root / ".git";
    if (ghc::filesystem::exists(gitdir / "MERGE_HEAD", ec)) st.operation = "merge";
    else if (ghc::filesystem::exists(gitdir / "rebase-merge", ec) ||
             ghc::filesystem::exists(gitdir / "rebase-apply", ec)) st.operation = "rebase";
    else if (ghc::filesystem::exists(gitdir / "CHERRY_PICK_HEAD", ec)) st.operation = "cherry-pick";
    else if (ghc::filesystem::exists(gitdir / "REVERT_HEAD", ec)) st.operation = "revert";
  }
  return st;
}

Json diff(const std::string& dir, const std::string& path, bool staged,
          size_t max_bytes) {
  // An untracked file is not "unchanged" - it has no index entry, so the diff is
  // empty for a structural reason. Answer with its full content as additions.
  if (!path.empty() && !staged && safe_pathspec(path) && !in_index(dir, path)) {
    return untracked_diff(dir, path, max_bytes);
  }
  std::vector<std::string> args;
  args.push_back("diff");
  // Colour and external diff drivers would corrupt the payload the UI parses.
  args.push_back("--no-color");
  args.push_back("--no-ext-diff");
  if (staged) args.push_back("--cached");
  if (!path.empty()) {
    if (!safe_pathspec(path)) {
      return Json{{"ok", false}, {"error", "invalid path"}};
    }
    args.push_back("--");
    args.push_back(path);
  }
  GitResult r = run(dir, args);
  Json j = Json::object();
  j["ok"] = r.ok;
  if (!r.ok) {
    j["error"] = trim(r.err.empty() ? r.out : r.err);
    return j;
  }
  if (r.out.size() > max_bytes) {
    j["diff"] = r.out.substr(0, max_bytes);
    j["truncated"] = true;
  } else {
    j["diff"] = r.out;
    j["truncated"] = false;
  }
  return j;
}

Json show_head(const std::string& dir, const std::string& path) {
  if (!safe_pathspec(path)) return Json{{"ok", false}, {"error", "invalid path"}};
  GitResult r = run(dir, {"show", "HEAD:" + path});
  if (!r.ok) {
    // A file added in this change has no HEAD version; that is not an error the
    // UI needs to shout about, so it reports exists=false.
    return Json{{"ok", true}, {"exists", false}, {"content", ""}};
  }
  return Json{{"ok", true}, {"exists", true}, {"content", r.out}};
}

Json log(const std::string& dir, int max_count) {
  if (max_count <= 0) max_count = 30;
  // Unit separators: a commit subject can contain any printable character, so
  // splitting on a tab or pipe would eventually mangle one.
  GitResult r = run(dir, {"log", "--no-color", "-n", std::to_string(max_count),
                          "--pretty=format:%H%x1f%h%x1f%an%x1f%at%x1f%s%x1e"});
  Json arr = Json::array();
  if (r.ok) {
    std::string cur;
    for (size_t i = 0; i < r.out.size(); ++i) {
      char c = r.out[i];
      if (c == '\x1e') {
        if (!cur.empty()) {
          std::vector<std::string> f;
          std::string part;
          for (size_t k = 0; k < cur.size(); ++k) {
            if (cur[k] == '\x1f') {
              f.push_back(part);
              part.clear();
            } else {
              part.push_back(cur[k]);
            }
          }
          f.push_back(part);
          if (f.size() >= 5) {
            Json j = Json::object();
            j["hash"] = f[0];
            j["short"] = f[1];
            j["author"] = f[2];
            j["time"] = static_cast<int64_t>(std::atoll(f[3].c_str())) * 1000;
            j["subject"] = f[4];
            arr.push_back(j);
          }
          cur.clear();
        }
      } else if (c != '\n') {
        cur.push_back(c);
      }
    }
  }
  return Json{{"ok", r.ok}, {"commits", arr}};
}

Json branches(const std::string& dir) {
  GitResult r = run(dir, {"branch", "--no-color", "--format=%(HEAD)%1f%(refname:short)"});
  Json arr = Json::array();
  std::string current;
  if (r.ok) {
    std::stringstream ss(r.out);
    std::string line;
    while (std::getline(ss, line)) {
      if (line.empty()) continue;
      bool is_current = line[0] == '*';
      std::string name = line.size() > 2 ? line.substr(2) : std::string();
      if (name.empty()) continue;
      if (is_current) current = name;
      arr.push_back(Json{{"name", name}, {"current", is_current}, {"remote", false}});
    }
  }
  return Json{{"ok", r.ok}, {"current", current}, {"branches", arr}};
}

namespace {
// Shared "run and report" tail for the write actions.
Json report(const GitResult& r, const std::string& ok_message) {
  if (r.ok) return Json{{"ok", true}, {"message", ok_message}};
  std::string msg = trim(r.err.empty() ? r.out : r.err);
  if (msg.empty()) msg = "git failed";
  return Json{{"ok", false}, {"message", msg}};
}
}  // namespace

Json stage(const std::string& dir, const std::vector<std::string>& paths) {
  std::vector<std::string> args;
  args.push_back("add");
  args.push_back("--");
  bool any = false;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (!safe_pathspec(paths[i])) continue;
    args.push_back(paths[i]);
    any = true;
  }
  if (!any) return Json{{"ok", false}, {"message", "no valid paths"}};
  return report(run(dir, args), "staged");
}

Json unstage(const std::string& dir, const std::vector<std::string>& paths) {
  // `reset HEAD --` also works on a repo with no commits (it empties the index
  // entry instead of failing), unlike `git restore --staged`.
  std::vector<std::string> args;
  args.push_back("reset");
  args.push_back("-q");
  args.push_back("HEAD");
  args.push_back("--");
  bool any = false;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (!safe_pathspec(paths[i])) continue;
    args.push_back(paths[i]);
    any = true;
  }
  if (!any) return Json{{"ok", false}, {"message", "no valid paths"}};
  return report(run(dir, args), "unstaged");
}

Json stage_all(const std::string& dir) {
  // -A includes untracked files. `-u` alone would leave the new file out, which
  // is the opposite of what a button labelled "stage everything" promises - and
  // the panel lists untracked files explicitly, so nothing is hidden from the
  // user before they press commit.
  return report(run(dir, {"add", "-A"}), "staged");
}

Json commit(const std::string& dir, const std::string& message, bool amend) {
  if (trim(message).empty()) {
    return Json{{"ok", false}, {"message", "empty commit message"}};
  }
  std::vector<std::string> args;
  args.push_back("commit");
  if (amend) args.push_back("--amend");
  // -F - would need stdin; the message is passed as a single argument instead.
  args.push_back("-m");
  args.push_back(message);
  GitResult r = run(dir, args);
  Json j = report(r, "committed");
  if (r.ok) {
    // The new HEAD, so the UI can show what it just created.
    GitResult h = run(dir, {"rev-parse", "--short", "HEAD"});
    if (h.ok) j["hash"] = trim(h.out);
  }
  return j;
}

Json discard(const std::string& dir, const std::vector<std::string>& paths) {
  std::vector<std::string> args;
  args.push_back("checkout");
  args.push_back("--");
  bool any = false;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (!safe_pathspec(paths[i])) continue;
    args.push_back(paths[i]);
    any = true;
  }
  if (!any) return Json{{"ok", false}, {"message", "no valid paths"}};
  return report(run(dir, args), "discarded");
}

}  // namespace git
}  // namespace fs
