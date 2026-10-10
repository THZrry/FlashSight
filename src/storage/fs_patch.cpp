// fs_patch.cpp - patch apply (forward/reverse)
#include "fs_patch.hpp"

#include <fstream>

namespace fs {

namespace {

// read whole file (text) with error; missing → returns false
bool read_text(const ghc::filesystem::path& p, std::string* out) {
  if (!ghc::filesystem::exists(p)) return false;
  std::ifstream ifs(p.string(), std::ios::binary);
  if (!ifs) return false;
  std::stringstream ss;
  ss << ifs.rdbuf();
  *out = ss.str();
  return true;
}

bool write_text(const ghc::filesystem::path& p, const std::string& content) {
  if (!p.parent_path().empty() && !ghc::filesystem::exists(p.parent_path())) {
    ghc::filesystem::create_directories(p.parent_path());
  }
  std::ofstream ofs(p.string(), std::ios::binary | std::ios::trunc);
  if (!ofs) return false;
  ofs << content;
  ofs.flush();
  return !!ofs;
}

std::string norm_rel(const std::string& p) {
  return ghc::filesystem::path(p).lexically_normal().generic_string();
}
}  // namespace

Json make_write_patch(const std::string& path, const std::string& before,
                      const std::string& after) {
  Json c = Json::object();
  c["op"] = "write";
  c["path"] = path;
  c["before"] = before;
  c["after"] = after;
  return Json{{"turnId", ""}, {"changes", Json::array({c})}};
}

FsResult apply_patch_forward(const ProjectStore& ps, const Json& patch) {
  // 正向：把 after 写盘（file.write 工具直接写盘后再留 patch；此处用于重建/测试）
  if (!patch.is_object() || !patch.contains("changes") || !patch["changes"].is_array()) {
    return FsResult::error(ErrCode::kInvalidArg, "invalid patch");
  }
  for (auto& c : patch["changes"]) {
    std::string op = jsonutil::get<std::string>(c, "op", "");
    // 只处理 write（其余留给工具层）
    if (op == "write") {
      std::string p = jsonutil::get<std::string>(c, "path", "");
      std::string after = jsonutil::get<std::string>(c, "after", "");
      ghc::filesystem::path full = ps.project_root / p;
      if (!write_text(full, after)) {
        return FsResult::error(ErrCode::kIo, "fwd write failed: " + p);
      }
    }
  }
  return FsResult::success();
}

FsResult apply_patch_reverse(const ProjectStore& ps, const Json& patch) {
  if (!patch.is_object() || !patch.contains("changes") || !patch["changes"].is_array()) {
    return FsResult::error(ErrCode::kInvalidArg, "invalid patch");
  }
  FsResult agg = FsResult::success();
  for (auto& c : patch["changes"]) {
    std::string op = jsonutil::get<std::string>(c, "op", "");
    std::string p = jsonutil::get<std::string>(c, "path", "");
    ghc::filesystem::path full = ps.project_root / p;
    if (op == "write") {
      // 当前内容若 == after 则写回 before；否则对不上 → 报错
      std::string cur;
      if (read_text(full, &cur)) {
        std::string after = jsonutil::get<std::string>(c, "after", "");
        if (cur == after) {
          std::string before = jsonutil::get<std::string>(c, "before", "");
          if (!write_text(full, before)) {
            agg = FsResult::error(ErrCode::kIo, "reverse write failed: " + p + " (file missing)");
          }
        } else {
          // 文件被修改过 → 不一致如实报错
          agg = FsResult::error(ErrCode::kRollbackConflict,
                                "rollback conflict (content changed): " + p);
        }
      } else {
        // 文件缺失：容忍（已被删）
        agg = FsResult::error(ErrCode::kRollbackConflict,
                              "rollback conflict (file missing): " + p);
      }
    } else if (op == "rm-new") {
      // 删除本应不存在的文件
      std::error_code ec;
      ghc::filesystem::remove(full, ec);
      // 缺失不算错
    } else if (op == "rm") {
      std::string before = jsonutil::get<std::string>(c, "before", "");
      if (before.empty()) {
        agg = FsResult::error(ErrCode::kRollbackConflict, "cannot undo rm without before: " + p);
      } else {
        if (!write_text(full, before)) {
          agg = FsResult::error(ErrCode::kIo, "restore rm failed: " + p);
        }
      }
    } else if (op == "rename") {
      std::string to = jsonutil::get<std::string>(c, "to", "");
      std::string from = jsonutil::get<std::string>(c, "from", "");
      ghc::filesystem::path src = ps.project_root / to;
      ghc::filesystem::path dst = ps.project_root / from;
      std::error_code ec;
      if (ghc::filesystem::exists(src, ec)) {
        ghc::filesystem::rename(src, dst, ec);
        if (ec) agg = FsResult::error(ErrCode::kIo, "rename reverse failed: " + ec.message());
      }
      // from 目标缺失：忽略
    } else if (op == "binary") {
      agg = FsResult::error(ErrCode::kRollbackConflict,
                            "binary file cannot be rolled back: " + p);
    }
    // 未知 op 忽略
  }
  return agg;
}

}  // namespace fs