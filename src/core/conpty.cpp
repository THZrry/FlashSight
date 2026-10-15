// conpty.cpp - ConPTY (Windows pseudo console) wrapper.
#include "conpty.hpp"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

// CreatePseudoConsole lives in kernel32 on 1809+; the headers only declare it
// when _WIN32_WINNT >= 0x0A00, which the build sets.
typedef HRESULT(WINAPI* CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD,
                                              HPCON*);
typedef void(WINAPI* ResizePseudoConsoleFn)(HPCON, COORD);
typedef void(WINAPI* ClosePseudoConsoleFn)(HPCON);

namespace fs {

namespace {

// UTF-8 <-> UTF-16 (the Win32 process API is wide-only)
std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                              nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

// Windows command-line quoting (the same rule the CRT parses).
std::wstring quote_arg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  for (size_t i = 0; i < a.size(); ++i) {
    size_t backslashes = 0;
    while (i < a.size() && a[i] == L'\\') {
      ++backslashes;
      ++i;
    }
    if (i == a.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (a[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(a[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

struct Api {
  CreatePseudoConsoleFn create = nullptr;
  ResizePseudoConsoleFn resize = nullptr;
  ClosePseudoConsoleFn close = nullptr;
  bool ok = false;
};

const Api& api() {
  static Api a = [] {
    Api r;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!k) return r;
    r.create = reinterpret_cast<CreatePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(k, "CreatePseudoConsole")));
    r.resize = reinterpret_cast<ResizePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(k, "ResizePseudoConsole")));
    r.close = reinterpret_cast<ClosePseudoConsoleFn>(
        reinterpret_cast<void*>(GetProcAddress(k, "ClosePseudoConsole")));
    r.ok = r.create && r.close;
    return r;
  }();
  return a;
}

}  // namespace

struct ConPty::Impl {
  HPCON pc = nullptr;
  HANDLE in_w = nullptr;   // we write here -> child's stdin
  HANDLE out_r = nullptr;  // we read here  <- child's stdout
  PROCESS_INFORMATION pi;
  bool started = false;
  bool exit_known = false;
  int exit_code = 0;
  std::string pending;  // leftover bytes from a partial read
};

bool ConPty::available() { return api().ok; }

ConPty::ConPty() : impl_(new Impl()) {
  std::memset(&impl_->pi, 0, sizeof(impl_->pi));
}

ConPty::~ConPty() {
  terminate();
  delete impl_;
}

bool ConPty::start(const std::string& exe, const std::vector<std::string>& args,
                   const std::string& cwd, int cols, int rows) {
  const Api& a = api();
  if (!a.ok) return false;

  SECURITY_ATTRIBUTES sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
  if (!CreatePipe(&in_r, &in_w, &sa, 0)) return false;
  if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
    CloseHandle(in_r);
    CloseHandle(in_w);
    return false;
  }
  // our ends must not be inherited by the child
  SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

  COORD size;
  size.X = static_cast<SHORT>(cols > 0 ? cols : 80);
  size.Y = static_cast<SHORT>(rows > 0 ? rows : 24);
  HRESULT hr = a.create(size, in_r, out_w, 0, &impl_->pc);
  if (FAILED(hr) || !impl_->pc) {
    CloseHandle(in_r);
    CloseHandle(in_w);
    CloseHandle(out_r);
    CloseHandle(out_w);
    return false;
  }

  STARTUPINFOEXW si;
  std::memset(&si, 0, sizeof(si));
  si.StartupInfo.cb = sizeof(si);
  // STARTF_USESTDHANDLES is REQUIRED, not optional: without it the child
  // inherits the parent's console std handles and keeps talking to *our*
  // console - the pseudo console then only ever emits its own startup
  // sequences, and the shell exits immediately (its stdin looks closed). The
  // handles themselves are left null: the pseudo console supplies them.
  si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nullptr;
  si.StartupInfo.hStdOutput = nullptr;
  si.StartupInfo.hStdError = nullptr;
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  si.lpAttributeList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(
          GetProcessHeap(), 0, attr_size));
  if (!si.lpAttributeList ||
      !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)) {
    if (si.lpAttributeList) HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    CloseHandle(in_r);
    CloseHandle(in_w);
    CloseHandle(out_r);
    CloseHandle(out_w);
    a.close(impl_->pc);
    impl_->pc = nullptr;
    return false;
  }
  UpdateProcThreadAttribute(si.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, impl_->pc,
                            sizeof(HPCON), nullptr, nullptr);

  std::wstring cmd = quote_arg(widen(exe));
  for (size_t i = 0; i < args.size(); ++i) {
    cmd += L' ';
    cmd += quote_arg(widen(args[i]));
  }
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(L'\0');
  std::wstring wcwd = widen(cwd);

  BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                           EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                           nullptr, wcwd.empty() ? nullptr : wcwd.c_str(),
                           &si.StartupInfo, &impl_->pi);
  DeleteProcThreadAttributeList(si.lpAttributeList);
  HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
  // the child holds its own ends now
  CloseHandle(in_r);
  CloseHandle(out_w);
  if (!ok) {
    CloseHandle(in_w);
    CloseHandle(out_r);
    a.close(impl_->pc);
    impl_->pc = nullptr;
    return false;
  }

  impl_->in_w = in_w;
  impl_->out_r = out_r;
  impl_->started = true;
  return true;
}

bool ConPty::running() const { return impl_ && impl_->started && !impl_->exit_known; }

bool ConPty::read(std::string* out) {
  if (!impl_ || !impl_->out_r) return false;
  char buf[8192];
  for (;;) {
    DWORD avail = 0;
    // PeekConsole-free non-blocking read: a pipe with no data would block, so
    // only read what is already there.
    if (!PeekNamedPipe(impl_->out_r, nullptr, 0, nullptr, &avail, nullptr)) {
      return false;
    }
    if (avail == 0) return true;
    DWORD want = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
    DWORD got = 0;
    if (!ReadFile(impl_->out_r, buf, want, &got, nullptr)) return false;
    if (got == 0) return true;
    if (out) out->append(buf, got);
  }
}

bool ConPty::write(const std::string& bytes) {
  if (!impl_ || !impl_->in_w || bytes.empty()) return false;
  DWORD wrote = 0;
  return WriteFile(impl_->in_w, bytes.data(), static_cast<DWORD>(bytes.size()),
                   &wrote, nullptr) != 0;
}

void ConPty::resize(int cols, int rows) {
  if (!impl_ || !impl_->pc || !api().resize) return;
  COORD c;
  c.X = static_cast<SHORT>(cols > 0 ? cols : 80);
  c.Y = static_cast<SHORT>(rows > 0 ? rows : 24);
  api().resize(impl_->pc, c);
}

bool ConPty::exited(int* code) {
  if (!impl_ || !impl_->started) return true;
  if (impl_->exit_known) {
    if (code) *code = impl_->exit_code;
    return true;
  }
  DWORD w = WaitForSingleObject(impl_->pi.hProcess, 0);
  if (w != WAIT_OBJECT_0) return false;
  DWORD ec = 0;
  GetExitCodeProcess(impl_->pi.hProcess, &ec);
  impl_->exit_code = static_cast<int>(ec);
  impl_->exit_known = true;
  if (code) *code = impl_->exit_code;
  return true;
}

void ConPty::terminate() {
  if (!impl_) return;
  if (impl_->pc) {
    // closing the pseudo console makes the attached child exit
    api().close(impl_->pc);
    impl_->pc = nullptr;
  }
  if (impl_->pi.hProcess) {
    if (WaitForSingleObject(impl_->pi.hProcess, 300) != WAIT_OBJECT_0) {
      TerminateProcess(impl_->pi.hProcess, 1);
      WaitForSingleObject(impl_->pi.hProcess, 500);
    }
    CloseHandle(impl_->pi.hThread);
    CloseHandle(impl_->pi.hProcess);
    std::memset(&impl_->pi, 0, sizeof(impl_->pi));
  }
  if (impl_->in_w) {
    CloseHandle(impl_->in_w);
    impl_->in_w = nullptr;
  }
  if (impl_->out_r) {
    CloseHandle(impl_->out_r);
    impl_->out_r = nullptr;
  }
  impl_->started = false;
}

}  // namespace fs

#else  // not Windows

namespace fs {

struct ConPty::Impl {};

bool ConPty::available() { return false; }
ConPty::ConPty() : impl_(nullptr) {}
ConPty::~ConPty() {}
bool ConPty::start(const std::string&, const std::vector<std::string>&,
                   const std::string&, int, int) {
  return false;
}
bool ConPty::running() const { return false; }
bool ConPty::read(std::string*) { return false; }
bool ConPty::write(const std::string&) { return false; }
void ConPty::resize(int, int) {}
bool ConPty::exited(int*) { return true; }
void ConPty::terminate() {}

}  // namespace fs

#endif
