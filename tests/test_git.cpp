// test_git.cpp - git CLI wrapper.
//
// These tests run against a REAL temporary repository: the whole point of the
// wrapper is to parse git's actual output, and a fake would only prove that the
// fake matches the parser. Every test skips itself when git is not installed
// (the CI machine may not have it) rather than failing.
#include "common/fs_git.hpp"
#include "test_framework.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

#include <ghc/filesystem.hpp>

using namespace fs::git;
using fs::Json;
using fs::jsonutil::get;

namespace {

bool have_git() {
#if defined(_WIN32)
  return std::system("git --version >nul 2>&1") == 0;
#else
  return std::system("git --version >/dev/null 2>&1") == 0;
#endif
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream f(path.c_str(), std::ios::binary);
  f << text;
}

std::string read_file(const std::string& path) {
  std::ifstream f(path.c_str(), std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s;
}

// Build a repo in `dir` and make one commit. Returns false when git is missing.
bool init_repo(const std::string& dir) {
  if (!have_git()) return false;
  ghc::filesystem::create_directories(dir);
  std::string q = "\"" + dir + "\"";
  std::string cmds =
      "git -C " + q + " init -q && "
      "git -C " + q + " config user.email t@example.com && "
      "git -C " + q + " config user.name Tester && "
      "git -C " + q + " config commit.gpgsign false && "
      // The tests compare file bytes; Windows git would otherwise rewrite line
      // endings on checkout and every "discard" assertion would fail on CRLF.
      "git -C " + q + " config core.autocrlf false";
  if (std::system(cmds.c_str()) != 0) return false;
  return true;
}

std::string run_in(const std::string& dir, const std::string& cmd) {
  std::string q = "\"" + dir + "\"";
  return "git -C " + q + " " + cmd;
}

}  // namespace

FS_TEST(git_not_a_repo_is_not_an_error) {
  fstest::TempDir tmp("git_norepo");
  Status st = status(tmp.path());
  EXPECT_FALSE(st.ok);
  EXPECT_FALSE(st.is_repo);
  EXPECT_FALSE(st.error.empty());  // "not a git repository" is shown to the user
  EXPECT_FALSE(is_repo(tmp.path()));
}

FS_TEST(git_status_parses_changes) {
  fstest::TempDir tmp("git_status");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "hello\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);

  Status clean = status(tmp.path());
  EXPECT_TRUE(clean.ok);
  EXPECT_TRUE(clean.is_repo);
  EXPECT_TRUE(clean.clean);
  EXPECT_FALSE(clean.branch.empty());

  write_file(tmp.path() + "/a.txt", "hello world\n");   // modified, unstaged
  write_file(tmp.path() + "/b.txt", "new\n");           // untracked
  Status st = status(tmp.path());
  ASSERT_TRUE(st.ok);
  EXPECT_FALSE(st.clean);
  ASSERT_TRUE(st.changes.size() == 2);
  bool saw_modified = false, saw_untracked = false;
  for (size_t i = 0; i < st.changes.size(); ++i) {
    if (st.changes[i].path == "a.txt") {
      saw_modified = true;
      EXPECT_EQ(st.changes[i].work_status, 'M');
      EXPECT_FALSE(st.changes[i].staged());
    }
    if (st.changes[i].path == "b.txt") {
      saw_untracked = true;
      EXPECT_TRUE(st.changes[i].untracked());
    }
  }
  EXPECT_TRUE(saw_modified);
  EXPECT_TRUE(saw_untracked);
  EXPECT_TRUE(st.operation.empty());
}

FS_TEST(git_diff_and_show_head) {
  fstest::TempDir tmp("git_diff");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "one\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);
  write_file(tmp.path() + "/a.txt", "one\ntwo\n");

  Json d = diff(tmp.path(), "a.txt", false);
  EXPECT_TRUE(get<bool>(d, "ok", false));
  std::string text = get<std::string>(d, "diff", "");
  EXPECT_TRUE(text.find("+two") != std::string::npos);

  // the index diff is empty until the change is staged
  Json staged = diff(tmp.path(), "a.txt", true);
  EXPECT_TRUE(get<std::string>(staged, "diff", "").empty());
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  Json staged2 = diff(tmp.path(), "a.txt", true);
  EXPECT_TRUE(get<std::string>(staged2, "diff", "").find("+two") !=
              std::string::npos);

  Json head = show_head(tmp.path(), "a.txt");
  EXPECT_TRUE(get<bool>(head, "exists", false));
  EXPECT_EQ(get<std::string>(head, "content", ""), std::string("one\n"));
  // a path that does not exist at HEAD reports exists=false, not an error
  Json missing = show_head(tmp.path(), "nope.txt");
  EXPECT_TRUE(get<bool>(missing, "ok", false));
  EXPECT_FALSE(get<bool>(missing, "exists", true));
}

FS_TEST(git_stage_commit_and_log) {
  fstest::TempDir tmp("git_commit");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "v1\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);

  write_file(tmp.path() + "/a.txt", "v2\n");
  write_file(tmp.path() + "/c.txt", "c\n");
  Json s = stage(tmp.path(), {"a.txt", "c.txt"});
  EXPECT_TRUE(get<bool>(s, "ok", false));
  Status st = status(tmp.path());
  bool a_staged = false, c_staged = false;
  for (size_t i = 0; i < st.changes.size(); ++i) {
    if (st.changes[i].path == "a.txt") a_staged = st.changes[i].staged();
    if (st.changes[i].path == "c.txt") c_staged = st.changes[i].staged();
  }
  EXPECT_TRUE(a_staged);
  EXPECT_TRUE(c_staged);

  // an empty message is refused before git runs
  Json bad = commit(tmp.path(), "   ");
  EXPECT_FALSE(get<bool>(bad, "ok", true));

  Json c = commit(tmp.path(), "second commit");
  EXPECT_TRUE(get<bool>(c, "ok", false));
  EXPECT_FALSE(get<std::string>(c, "hash", "").empty());
  EXPECT_TRUE(status(tmp.path()).clean);

  Json lg = log(tmp.path(), 10);
  EXPECT_TRUE(get<bool>(lg, "ok", false));
  const Json& commits = lg["commits"];
  ASSERT_TRUE(commits.is_array() && commits.size() == 2);
  EXPECT_EQ(get<std::string>(commits[0], "subject", ""),
            std::string("second commit"));
  EXPECT_EQ(get<std::string>(commits[1], "subject", ""),
            std::string("first"));
}

FS_TEST(git_unstage_and_discard) {
  fstest::TempDir tmp("git_unstage");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "v1\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);

  write_file(tmp.path() + "/a.txt", "v2\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  Json u = unstage(tmp.path(), {"a.txt"});
  EXPECT_TRUE(get<bool>(u, "ok", false));
  Status st = status(tmp.path());
  ASSERT_TRUE(st.changes.size() == 1);
  EXPECT_FALSE(st.changes[0].staged());
  EXPECT_EQ(st.changes[0].work_status, 'M');

  // discarding restores the committed content
  Json d = discard(tmp.path(), {"a.txt"});
  EXPECT_TRUE(get<bool>(d, "ok", false));
  EXPECT_EQ(read_file(tmp.path() + "/a.txt"), std::string("v1\n"));
  EXPECT_TRUE(status(tmp.path()).clean);
}

FS_TEST(git_rejects_path_traversal_and_flags) {
  fstest::TempDir tmp("git_safety");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "v1\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);

  // A path that looks like an option must never reach git as an option, and a
  // traversal must be refused outright.
  Json r = stage(tmp.path(), {"--force", "../outside"});
  EXPECT_FALSE(get<bool>(r, "ok", true));
  Json d = diff(tmp.path(), "--no-index", false);
  EXPECT_FALSE(get<bool>(d, "ok", true));
}

FS_TEST(git_branches) {
  fstest::TempDir tmp("git_branch");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "v1\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);
  std::system(run_in(tmp.path(), "branch feature").c_str());

  Json b = branches(tmp.path());
  EXPECT_TRUE(get<bool>(b, "ok", false));
  const Json& arr = b["branches"];
  ASSERT_TRUE(arr.is_array() && arr.size() == 2);
  bool saw_current = false, saw_feature = false;
  for (size_t i = 0; i < arr.size(); ++i) {
    std::string name = get<std::string>(arr[i], "name", "");
    if (get<bool>(arr[i], "current", false)) saw_current = true;
    if (name == "feature") saw_feature = true;
  }
  EXPECT_TRUE(saw_current);
  EXPECT_TRUE(saw_feature);
  EXPECT_FALSE(get<std::string>(b, "current", "").empty());
}

FS_TEST(git_untracked_file_has_an_all_added_diff) {
  fstest::TempDir tmp("git_untracked");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "v1\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);
  write_file(tmp.path() + "/fresh.txt", "alpha\nbeta\n");

  // `git diff` against an untracked file is empty; the panel must still show
  // what the file contains, as additions.
  Json d = diff(tmp.path(), "fresh.txt", false);
  EXPECT_TRUE(get<bool>(d, "ok", false));
  std::string text = get<std::string>(d, "diff", "");
  EXPECT_TRUE(text.find("+alpha") != std::string::npos);
  EXPECT_TRUE(text.find("+beta") != std::string::npos);
  EXPECT_TRUE(text.find("new file") != std::string::npos);
}

FS_TEST(git_stage_all_includes_untracked) {
  fstest::TempDir tmp("git_stage_all");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/a.txt", "v1\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add a.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m first").c_str()) == 0);
  write_file(tmp.path() + "/a.txt", "v2\n");
  write_file(tmp.path() + "/brand-new.txt", "n\n");

  Json r = stage_all(tmp.path());
  EXPECT_TRUE(get<bool>(r, "ok", false));
  Status st = status(tmp.path());
  ASSERT_TRUE(st.changes.size() == 2);
  for (size_t i = 0; i < st.changes.size(); ++i) {
    // "stage everything" must stage everything the panel listed, untracked
    // files included - otherwise the commit silently misses them.
    EXPECT_TRUE(st.changes[i].staged());
  }
}

FS_TEST(git_diff_is_truncated) {
  fstest::TempDir tmp("git_trunc");
  ASSERT_TRUE(init_repo(tmp.path()));
  write_file(tmp.path() + "/big.txt", "seed\n");
  ASSERT_TRUE(std::system(run_in(tmp.path(), "add big.txt").c_str()) == 0);
  ASSERT_TRUE(std::system(run_in(tmp.path(), "commit -q -m big").c_str()) == 0);
  // A big CHANGE, not a big file: truncation is about the diff payload, and
  // appending one line to a huge file still produces a tiny diff.
  std::string changed;
  for (int i = 0; i < 5000; ++i) changed += "line " + std::to_string(i) + "\n";
  write_file(tmp.path() + "/big.txt", changed);

  Json d = diff(tmp.path(), "big.txt", false, 1024);
  EXPECT_TRUE(get<bool>(d, "ok", false));
  EXPECT_TRUE(get<bool>(d, "truncated", false));
  EXPECT_TRUE(get<std::string>(d, "diff", "").size() <= 1024);
}
