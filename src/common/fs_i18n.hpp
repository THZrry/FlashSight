// fs_i18n.hpp - i18n infrastructure (00-global §common, ui-events.md §3)
// 语言表：编译时预处理进 cpp；v1 中英两份，源码直接给 key → 文案。
// 预留外部翻译渠道（加载外部文件优先于编译内嵌）。
#pragma once

#include <string>

namespace fs {
namespace i18n {

enum class Lang { kAuto = 0, kZh, kEn };

// 语言标签（UI 显示 "中文"/"English" 等）
const char* lang_tag(Lang l);

// 当前有效语言（Auto → 跟随系统 locale）
Lang effective_lang();

// 查文案：key → 文案（找不到返回 key 原样）
std::string tr(const std::string& key);

// 语言表注册（编译内嵌 + 外部覆盖）；P4 落地
void set_override_lang(Lang l);

}  // namespace i18n
}  // namespace fs