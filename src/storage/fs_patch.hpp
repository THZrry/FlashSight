// fs_patch.hpp - patch application for file changes (session-history.md §5)
#pragma once

#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "fs_error.hpp"
#include "fs_json.hpp"
#include "storage/fs_store.hpp"

namespace fs {

// patch 文件 schema（每轮一个，patches/<turnId>.json）
// {
//   "changes": [
//     {"op":"write", "path":"...", "before":"...", "after":"..."},     // 文本写
//     {"op":"rm",    "path":"..."},                                    // 删除（保留 before 快照）
//     {"op":"rm-new","path":"..."},                                    // 删除新创建的文件
//     {"op":"rename","from":"...", "to":"...", "before":"..."},        // 重命名
//     {"op":"binary","path":"...", "deleted":false}                    // 二进制不可逆 → 回滚报错
//   ],
//   "turnId": "..."   // 冗余字段便于自检
// }

// 正向应用 patch（把 after 落到磁盘；用于 send 后落库——本函数不改变 patch 内容）
// 实际上 file.write 工具直接写盘，patch 仅留记录；这里提供正向应用便于重建场景
FsResult apply_patch_forward(const ProjectStore& ps, const Json& patch);

// 逆向还原（回滚用）：从磁盘当前状态还原到 before。
// 规则：write → 把 after 换回 before；rm → 重建 before（缺 before 则报错）；
// rm-new → 删除文件（本来就没有）；rename → 名字换回 from；
// binary 对不上 → 报错（如实反映，不静默）；缺文件容忍不报错。
FsResult apply_patch_reverse(const ProjectStore& ps, const Json& patch);

// 构造一个 write 的 patch 记录
Json make_write_patch(const std::string& path, const std::string& before,
                      const std::string& after);

}  // namespace fs