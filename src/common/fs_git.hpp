// fs_git.hpp - Git integration over the git CLI (zero library dependency).
//
// Why the CLI and not libgit2: git is already installed wherever a repository
// is, the CLI is the same thing the user would type, and linking libgit2 would
// add a large dependency (plus its own TLS/OpenSSL story) for a feature that is
// mostly "show me what changed". Every command runs with an explicit -C <repo>
// and a fixed argument vector, so a path can never be interpreted as a flag.
//
// The API is deliberately READ-ONLY except for the four obvious write actions
// (stage / unstage / commit / discard), each of which is its own method so the
// UI can put a confirmation in front of exactly the destructive one.
#pragma once

#include <string>
#include <vector>

#include "fs_json.hpp"

namespace fs {
namespace git {

// Result of running one git command. `ok` means "exit code 0"; a command that
// ran and failed keeps its stderr, because "fatal: not a git repository" is
// information the UI has to show rather than swallow.
struct GitResult {
  bool ok = false;
  int code = -1;
  std::string out;   // stdout
  std::string err;   // stderr
  bool timed_out = false;
  // false when the git executable itself could not be started at all (as
  // opposed to git running and refusing) - the UI shows a different message.
  bool available = true;
};

// Is `dir` inside a git work tree? Cheap enough to call before every panel
// refresh; the caller caches the answer per project.
bool is_repo(const std::string& dir);

// Top level of the work tree containing `dir` ("" when not a repo).
std::string repo_root(const std::string& dir);

// One changed path from `git status --porcelain=v1 -z`.
struct FileChange {
  std::string path;      // repo-relative, forward slashes
  std::string orig_path; // for renames/copies ("" otherwise)
  char index_status = ' ';   // staged column (A/M/D/R/C/U)
  char work_status = ' ';    // unstaged column (M/D/?/...)
  bool staged() const { return index_status != ' ' && index_status != '?'; }
  bool untracked() const { return index_status == '?' || work_status == '?'; }
  bool conflicted() const { return index_status == 'U' || work_status == 'U'; }
};

struct Status {
  bool ok = false;
  bool is_repo = false;
  std::string root;
  std::string branch;        // "" when detached
  std::string upstream;      // "" when none
  int ahead = 0;
  int behind = 0;
  bool detached = false;
  bool clean = true;
  // true while a merge/rebase/cherry-pick is in progress: the UI must say so
  // instead of pretending a plain commit is possible.
  std::string operation;     // "" | "merge" | "rebase" | "cherry-pick" | ...
  std::vector<FileChange> changes;
  std::string error;         // why ok == false
};

// `git status --porcelain=v1 -z --branch`, parsed into the struct above.
Status status(const std::string& dir);

// Unified diff. `path` empty = the whole repo. `staged` selects the index diff
// (what a commit would record) instead of the work-tree diff.
//   { ok, diff, truncated }
// `max_bytes` guards the UI: a diff can be megabytes and the panel has to stay
// responsive, so the caller gets a prefix and a flag rather than a hang.
Json diff(const std::string& dir, const std::string& path, bool staged,
          size_t max_bytes = 512 * 1024);

// The file's full text at HEAD (for a side-by-side / "before" view).
// -> { ok, content, exists }
Json show_head(const std::string& dir, const std::string& path);

// `git log --max-count=N` in a compact form.
// -> { ok, commits:[{hash, short, author, time, subject}] }
Json log(const std::string& dir, int max_count);

// Branches: { ok, current, branches:[{name, current, remote}] }
Json branches(const std::string& dir);

// ---- write actions ----
// All return { ok, message } where `message` is git's own output on failure.
Json stage(const std::string& dir, const std::vector<std::string>& paths);
Json unstage(const std::string& dir, const std::vector<std::string>& paths);
// Stage every tracked change (`git add -u`). Untracked files are NOT included:
// silently committing a stray build artifact is worse than an incomplete commit.
Json stage_all(const std::string& dir);
// Commit what is staged. `amend` rewrites the previous commit.
Json commit(const std::string& dir, const std::string& message, bool amend = false);
// Throw away work-tree changes for `paths` (`git checkout -- <paths>`).
// Destructive and not undoable - the UI must confirm first.
Json discard(const std::string& dir, const std::vector<std::string>& paths);

}  // namespace git
}  // namespace fs
