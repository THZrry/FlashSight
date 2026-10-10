// fs_path.hpp - path utilities on ghsl filesystem
// 00-global §common：join/normalize/expandUser，Win %USERPROFILE% 与 Linux ~ 统一
#pragma once

#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

namespace fs {

// 路径类型别名（全项目统一用；ghc 的路径类型在 ghc::filesystem 命名空间）
using FsPath = ::ghc::filesystem::path;

namespace pathutil {

// 展开 ~ 与 %USERPROFILE% → 绝对路径；不做符号链接解析
FsPath expand_user(const FsPath& p);

// 规范化：绝对化 + lexically_normal + 统一分隔符
FsPath normalize(const FsPath& p);

// 判断 child 是否位于 parent 目录下（词法级别）
bool is_within(const FsPath& child, const FsPath& parent);

// 相对化（供 UI/日志展示）
std::string display(const FsPath& p);

// 主目录：Win %USERPROFILE%（回退 %HOMEDRIVE%%HOMEPATH%），Linux $HOME
FsPath home_dir();

// 全局数据目录：~/.flashsight（session-history.md §1）
FsPath global_dir();

// 项目数据目录：<project>/.flashsight
FsPath project_dir(const FsPath& project_path);

// 会话数据目录：<project>/.flashsight/sessions/<sid>
FsPath session_dir(const FsPath& project_path, const std::string& sid);

}  // namespace pathutil
}  // namespace fs