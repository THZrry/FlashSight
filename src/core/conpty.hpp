// conpty.hpp - real interactive terminal on Windows (ConPTY).
//
// Pipes are not a terminal. A shell started on pipes has no console: it cannot
// see the window size, prints no prompt the way a user expects, and any
// full-screen program (vim, less, top) fails outright. Windows 10 1809+ exposes
// a *pseudo console* (ConPTY): the child gets a real console, and we get a VT
// byte stream in and keystrokes out - exactly the two halves the UI needs.
//
// This is the Win32 counterpart of the terminal model (fs_vt): ConPTY produces
// the VT stream, fs_vt::Screen turns it into a grid, and keys go back through
// write().
//
// On POSIX this reports unavailable and TerminalManager falls back to pipes.
#pragma once

#include <string>
#include <vector>

namespace fs {

class ConPty {
 public:
  // ConPTY needs Windows 10 1809 (build 17763) or newer.
  static bool available();

  ConPty();
  ~ConPty();

  // Start `exe` with `args` in `cwd` (empty = inherit) on a fresh pseudo
  // console of cols x rows. Returns false when ConPTY is unavailable or the
  // process could not be created.
  bool start(const std::string& exe, const std::vector<std::string>& args,
             const std::string& cwd, int cols, int rows);

  bool running() const;
  // Drain whatever the child has written (non-blocking). Returns false only on
  // a hard read error.
  bool read(std::string* out);
  // Send raw bytes to the child's stdin (keys are already VT-encoded).
  bool write(const std::string& bytes);
  // Tell the child the console size changed (full-screen apps listen for this).
  void resize(int cols, int rows);
  // Non-blocking exit check; sets *code when the child has exited.
  bool exited(int* code);
  // Close the pseudo console: the child sees the console close and exits.
  void terminate();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace fs
