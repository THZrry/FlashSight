// folder_picker.hpp - native folder helpers (desktop server side).
// The UI runs in a browser, which cannot hand out or receive OS paths, so the
// local exe performs these operations itself:
//   * pick_folder_dialog : "choose a folder" dialog (Windows IFileDialog)
//   * reveal_in_file_manager : open a folder in Explorer / Finder
//   * find_folder_by_name : resolve a *dropped folder name* (browsers only
//     expose the name, not the path) by searching likely roots
//   * default_search_roots : candidate roots for the drop resolution
#pragma once

#include <string>
#include <vector>

namespace fs {

// Shows a folder chooser. Returns true and sets *out on OK; false on cancel
// or when the platform has no picker.
bool pick_folder_dialog(std::string* out);

// Shows a FILE chooser (same dialog, no FOS_PICKFOLDERS). `filter_desc` /
// `filter_spec` are an optional Windows filter pair, e.g. ("可执行文件",
// "*.exe;*.cmd;*.bat") - needed for the browser driver and CLI tool paths, where
// a folder picker is simply the wrong control.
bool pick_file_dialog(std::string* out, const std::string& filter_desc = "",
                      const std::string& filter_spec = "");

// Opens `path` in the OS file manager. Returns false on failure.
bool reveal_in_file_manager(const std::string& path);

// Opens an http(s) URL in the user's default browser. Used by markdown links in
// chat: the UI has no address bar of its own, and navigating the app window away
// from itself would destroy the session view.
bool open_url_in_browser(const std::string& url);

// Opens one file with a specific program (the "open with" dropdown). `app` is a
// template like "code" / "notepad" / "" (system default).
bool open_path_with(const std::string& path, const std::string& app);

// Roots worth searching for a dropped folder: standard user folders and the
// fixed drive roots (Windows). Order = priority.
std::vector<std::string> default_search_roots();

// Breadth-first search for a directory named `name` (exact, case-insensitive
// on Windows) under any of `roots`, descending at most `depth` levels.
// Common noise dirs (.git, node_modules, Windows, ...) are skipped.
// Returns the shortest matching path found; empty when none.
std::string find_folder_by_name(const std::vector<std::string>& roots,
                                const std::string& name, int depth);

}  // namespace fs
