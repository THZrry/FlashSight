// fs_serialize.cpp - enum/string conversions and id generation (small)
#include "fs_serialize.hpp"

namespace fs {

const char* file_perm_str(FilePerm p) {
  switch (p) {
    case FilePerm::kApprove: return "approve";
    case FilePerm::kPermissive: return "permissive";
    case FilePerm::kFull: return "full";
  }
  return "permissive";
}

bool file_perm_from_str(const std::string& s, FilePerm* out) {
  if (!out) return false;
  if (s == "approve") { *out = FilePerm::kApprove; return true; }
  if (s == "permissive") { *out = FilePerm::kPermissive; return true; }
  if (s == "full") { *out = FilePerm::kFull; return true; }
  return false;
}

const char* enter_send_str(EnterSend e) {
  switch (e) {
    case EnterSend::kEnter: return "enter";
    case EnterSend::kCtrlEnter: return "ctrlEnter";
  }
  return "enter";
}

EnterSend enter_send_from_str(const std::string& s, EnterSend def) {
  if (s == "ctrlEnter") return EnterSend::kCtrlEnter;
  if (s == "enter") return EnterSend::kEnter;
  return def;
}

const char* term_kind_str(TermKind k) {
  switch (k) {
    case TermKind::kUser: return "user";
    case TermKind::kModelFg: return "model-fg";
    case TermKind::kModelBg: return "model-bg";
  }
  return "user";
}

const char* term_status_str(TermStatus s) {
  switch (s) {
    case TermStatus::kRunning: return "running";
    case TermStatus::kExited: return "exited";
    case TermStatus::kKilled: return "killed";
  }
  return "running";
}

}  // namespace fs