// fs_error.hpp - error codes & structured denial protocol
// arch.md §6: 错误码统一 common/ 错误类型；拒绝类错误带机器可读原因（给模型换道用）。
// permissions.md §4: {allowed:false, reason:"approve|lock|deny-list", lockedBy?, howToProceed}
#pragma once

#include <string>
#include <utility>

#include "nlohmann/json.hpp"

namespace fs {

// 机器可读原因（给模型换道用）；展示文案由 UI 层 i18n 负责
enum class ErrCode {
  kOk = 0,
  kNotFound,        // 路径/会话/项目/记录不存在
  kExists,          // 已存在（新建冲突）
  kInvalidArg,      // 参数非法（含 JSON schema 不符）
  kNoToolUse,       // 模型不支持 toolUse，硬拒绝进会话（provider.md §4）
  kConfirmRequired, // 低→高权限切换需要显式确认（permissions.md §1）
  kPermissionDenied,// 权限档拒绝（approve 门：等批准）
  kLocked,          // 写锁被其他会话持有（permissions.md §2）
  kDenyList,        // 命中黑名单（permissions.md §3）
  kDeferred,        // 自动模式不确定上下文下高风险动作降级（agent-loop.md §3）
  kRollbackConflict,// 回滚遇到二进制/重命名对不上（session-history.md §5）
  kSubagentDepth,   // 子代理深度超限（agent-loop.md §6）
  kCancelled,       // 用户取消
  kTimeout,         // 超时
  kIo,              // 文件系统错误
  kParse,           // JSON/JSONL 坏行等解析错误（宽容读时跳过不算错）
  kNetwork,         // 网络错误
  kRemote,          // provider 远程错误（状态码非 2xx）
  kBusy,            // 状态机忙/不允许该动作（如自动模式不允许 ask）
  kNotSupported,    // 未实现/不支持（v1 灰色项）
  kInternal,        // 内部错误（兜底）
};

// 结构化拒绝（permissions.md §4）：任何一扇门拒绝都返回这个
struct Denial {
  bool allowed = false;
  std::string reason;       // "approve" | "lock" | "deny-list" | "deferred" | "no-tool-use" | "confirm-required"
  std::string lockedBy;     // lock 时的持有会话 sid
  std::string howToProceed; // 机器可读换道建议
};

// 统一结果：要么带错误码，要么带结构化拒绝
struct FsResult {
  ErrCode code = ErrCode::kOk;
  Denial denial;                    // 仅 code 为拒绝类时有效
  std::string message;              // 面向用户/日志的可读信息（可选）
  nlohmann::json data = nlohmann::json::object(); // 成功时的载荷

  bool ok() const { return code == ErrCode::kOk; }
  bool denied() const {
    return code == ErrCode::kPermissionDenied || code == ErrCode::kLocked ||
           code == ErrCode::kDenyList || code == ErrCode::kDeferred ||
           code == ErrCode::kConfirmRequired;
  }

  static FsResult success(nlohmann::json data = nlohmann::json::object(),
                          std::string message = "") {
    FsResult r;
    r.data = std::move(data);
    r.message = std::move(message);
    return r;
  }
  static FsResult error(ErrCode code, std::string message = "") {
    FsResult r;
    r.code = code;
    r.message = std::move(message);
    return r;
  }
  static FsResult deny(std::string reason, std::string howToProceed = "",
                       std::string lockedBy = "") {
    FsResult r;
    r.code = reason == "lock" ? ErrCode::kLocked
           : reason == "deny-list" ? ErrCode::kDenyList
           : reason == "deferred" ? ErrCode::kDeferred
           : reason == "no-tool-use" ? ErrCode::kNoToolUse
           : reason == "confirm-required" ? ErrCode::kConfirmRequired
           : ErrCode::kPermissionDenied;
    r.denial.allowed = false;
    r.denial.reason = std::move(reason);
    r.denial.lockedBy = std::move(lockedBy);
    r.denial.howToProceed = std::move(howToProceed);
    return r;
  }
};

inline const char* err_code_name(ErrCode c) {
  switch (c) {
    case ErrCode::kOk: return "ok";
    case ErrCode::kNotFound: return "not-found";
    case ErrCode::kExists: return "exists";
    case ErrCode::kInvalidArg: return "invalid-arg";
    case ErrCode::kNoToolUse: return "no-tool-use";
    case ErrCode::kConfirmRequired: return "confirm-required";
    case ErrCode::kPermissionDenied: return "permission-denied";
    case ErrCode::kLocked: return "locked";
    case ErrCode::kDenyList: return "deny-list";
    case ErrCode::kDeferred: return "deferred";
    case ErrCode::kRollbackConflict: return "rollback-conflict";
    case ErrCode::kSubagentDepth: return "subagent-depth";
    case ErrCode::kCancelled: return "cancelled";
    case ErrCode::kTimeout: return "timeout";
    case ErrCode::kIo: return "io";
    case ErrCode::kParse: return "parse";
    case ErrCode::kNetwork: return "network";
    case ErrCode::kRemote: return "remote";
    case ErrCode::kBusy: return "busy";
    case ErrCode::kNotSupported: return "not-supported";
    case ErrCode::kInternal: return "internal";
  }
  return "unknown";
}

}  // namespace fs