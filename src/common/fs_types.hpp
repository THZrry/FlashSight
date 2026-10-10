// fs_types.hpp - shared enums & identifiers (used by storage/core/agent/provider)
// Serialized into JSON by fs_serialize helpers.
#pragma once

#include <string>

namespace fs {

// 文件三档权限（permissions.md §1）
enum class FilePerm { kApprove, kPermissive, kFull };

// 权限档默认（新建会话的缺省）
constexpr FilePerm kDefaultFilePerm = FilePerm::kPermissive;

// 会话发送键
enum class EnterSend { kEnter, kCtrlEnter };

// 会话模式
enum class Mode { kManual, kAuto };

// 终端种类（terminal.md §1）
enum class TermKind { kUser, kModelFg, kModelBg };

// 终端状态（terminal.md §2）
enum class TermStatus { kRunning, kExited, kKilled };

}  // namespace fs