// folder_picker.cpp - Windows folder dialog + folder search helpers.
#include "folder_picker.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <ghc/filesystem.hpp>

namespace fs {

bool pick_folder_dialog(std::string* out) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                           COINIT_DISABLE_OLE1DDE);
  bool com_owned = SUCCEEDED(hr);
  if (hr == RPC_E_CHANGED_MODE) com_owned = false;  // already STA/MTA: reuse

  bool ok = false;
  IFileDialog* dlg = nullptr;
  hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&dlg));
  if (SUCCEEDED(hr)) {
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
      dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                      FOS_PATHMUSTEXIST);
    }
    hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
          int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0,
                                        nullptr, nullptr);
          if (len > 0) {
            std::string buf(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path, -1, &buf[0], len, nullptr,
                                nullptr);
            buf.resize(static_cast<size_t>(len - 1));  // drop the NUL
            if (out) *out = buf;
            ok = true;
          }
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (com_owned) CoUninitialize();
  return ok;
}

bool pick_file_dialog(std::string* out, const std::string& filter_desc,
                      const std::string& filter_spec) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                           COINIT_DISABLE_OLE1DDE);
  bool com_owned = SUCCEEDED(hr);
  if (hr == RPC_E_CHANGED_MODE) com_owned = false;

  bool ok = false;
  IFileDialog* dlg = nullptr;
  hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&dlg));
  if (SUCCEEDED(hr)) {
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
      // NOTE: no FOS_PICKFOLDERS - that flag is the only difference from the
      // folder dialog, and with it the same dialog picks files instead.
      dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                      FOS_PATHMUSTEXIST);
    }
    if (!filter_spec.empty()) {
      // COMDLG_FILTERSPEC wants wide strings; convert from UTF-8.
      auto widen = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        if (!w.empty()) w.resize(w.size() - 1);
        return w;
      };
      std::wstring wd = widen(filter_desc.empty() ? "文件" : filter_desc);
      std::wstring ws = widen(filter_spec);
      COMDLG_FILTERSPEC spec[1];
      spec[0].pszName = wd.c_str();
      spec[0].pszSpec = ws.c_str();
      dlg->SetFileTypes(1, spec);
    }
    hr = dlg->Show(nullptr);
    if (SUCCEEDED(hr)) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
          int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0,
                                        nullptr, nullptr);
          if (len > 0) {
            std::string buf(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path, -1, &buf[0], len, nullptr,
                                nullptr);
            buf.resize(static_cast<size_t>(len - 1));
            if (out) *out = buf;
            ok = true;
          }
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (com_owned) CoUninitialize();
  return ok;
}

bool reveal_in_file_manager(const std::string& path) {
  if (path.empty()) return false;
  // explorer has quirky exit codes; fire and forget
  std::string cmd = "explorer \"" + path + "\"";
  return std::system(cmd.c_str()) != -1;
}

std::vector<std::string> default_search_roots() {
  std::vector<std::string> roots;
  char buf[MAX_PATH];
  // userprofile (C:\Users\X)
  if (GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH) > 0) {
    roots.push_back(buf);
  }
  // drive roots (C: through Z:)
  DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (drives & (1u << i)) {
      std::string root;
      root += static_cast<char>('A' + i);
      root += ":\\";
      UINT type = GetDriveTypeA(root.c_str());
      if (type == DRIVE_FIXED) roots.push_back(root);  // skip remotes/cdrom
    }
  }
  return roots;
}

namespace {

std::string lower_ascii(const std::string& s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return o;
}

bool is_noise_dir(const std::string& name) {
  static const char* noise[] = {
      ".git", ".svn", ".hg", "node_modules", "windows", "program files",
      "program files (x86)", "appdata", "programdata", "system volume information",
      "$recycle.bin", ".cache", ".vscode", ".venv", "venv", "dist", "build",
  };
  std::string l = lower_ascii(name);
  for (size_t i = 0; i < sizeof(noise) / sizeof(noise[0]); ++i) {
    if (l == noise[i]) return true;
  }
  return false;
}

}  // namespace

std::string find_folder_by_name(const std::vector<std::string>& roots,
                                const std::string& name, int depth) {
  if (name.empty() || depth < 0) return "";
  std::string want = lower_ascii(name);
  std::vector<std::pair<std::string, int> > level;
  for (size_t i = 0; i < roots.size(); ++i) {
    level.push_back(std::make_pair(roots[i], 0));
  }
  // BFS over roots in parallel so shallow hits beat deep ones globally
  while (!level.empty()) {
    std::vector<std::pair<std::string, int> > next;
    for (size_t i = 0; i < level.size(); ++i) {
      const std::string& dir = level[i].first;
      int d = level[i].second;
      std::error_code ec;
      ghc::filesystem::directory_iterator it(dir, ec), end;
      if (ec) continue;
      for (; it != end; it.increment(ec)) {
        if (ec) break;
        ghc::filesystem::path p = it->path();
        std::string fname = p.filename().string();
        bool is_dir = ghc::filesystem::is_directory(p, ec);
        if (!is_dir) continue;
        if (lower_ascii(fname) == want) return p.string();
        if (d + 1 <= depth && !is_noise_dir(fname)) {
          next.push_back(std::make_pair(p.string(), d + 1));
        }
      }
    }
    level.swap(next);
  }
  return "";
}

}  // namespace fs

#else  // not Windows

#include <cstdlib>

namespace fs {

bool pick_folder_dialog(std::string*) { return false; }

bool pick_file_dialog(std::string*, const std::string&, const std::string&) {
  return false;
}

bool reveal_in_file_manager(const std::string& path) {
  if (path.empty()) return false;
  return std::system(("xdg-open \"" + path + "\" >/dev/null 2>&1 &").c_str()) != -1;
}

std::vector<std::string> default_search_roots() {
  std::vector<std::string> roots;
  if (const char* home = std::getenv("HOME")) roots.push_back(home);
  roots.push_back("/");
  return roots;
}

std::string find_folder_by_name(const std::vector<std::string>&,
                                const std::string&, int) {
  return "";
}

}  // namespace fs

#endif
