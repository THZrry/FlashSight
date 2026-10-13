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

// Opens `path` in the OS file manager. Returns false on failure.
bool reveal_in_file_manager(const std::string& path);

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
