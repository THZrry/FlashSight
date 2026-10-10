// fs_serialize.hpp - enum <-> string & id helpers
#pragma once

#include <string>

#include "fs_types.hpp"

namespace fs {

// FilePerm
const char* file_perm_str(FilePerm p);
bool file_perm_from_str(const std::string& s, FilePerm* out);
// 权限字符串 "approve|permissive|full"
inline FilePerm file_perm_from_str(const std::string& s, FilePerm def) {
  FilePerm out = def;
  return file_perm_from_str(s, &out) ? out : def;
}

const char* enter_send_str(EnterSend e);
EnterSend enter_send_from_str(const std::string& s, EnterSend def);

const char* term_kind_str(TermKind k);
const char* term_status_str(TermStatus s);

}  // namespace fs