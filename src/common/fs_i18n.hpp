// fs_i18n.hpp - i18n infrastructure (00-global §common, ui-events.md §3)
//
// 源码语言是中文：UI 里直接写中文文案，并把它当作查表 key。
//   * 中文界面：tr() 原样返回 key（表里不需要条目，永远不会有 "缺翻译" 的丑陋回退）
//   * 英文界面：查 en 表；缺失时回退到中文原文（可读，不是 key 泄漏）
// 这样迁移是增量、安全的：任何一处想接翻译，套上 tr("...") 即可。
// 语言来源优先级：settings ui.language（显式）> 系统/浏览器 locale（auto）。
// 预留外部翻译文件覆盖（P4：加载外部 json 优先于编译内嵌）。
#pragma once

#include <string>

namespace fs {
namespace i18n {

enum class Lang { kAuto = 0, kZh, kEn };

// 语言标签（UI 显示 "中文"/"English"/"跟随系统"）
const char* lang_tag(Lang l);

// settings 值（"auto|zh|en"）<-> Lang
Lang lang_from_setting(const std::string& v);
const char* setting_from_lang(Lang l);

// 当前有效语言（Auto → 跟随系统 locale）
Lang effective_lang();

// 显式设定语言（settings 里改语言时调用；kAuto 恢复跟随系统）
void set_lang(Lang l);
// 兼容旧名（P4 落地前就存在的调用点）
void set_override_lang(Lang l);

// 系统 locale 推断（Windows: GetUserDefaultUILanguage；wasm: navigator.language）
Lang system_lang();

// 查文案：中文 key → 当前语言文案（找不到返回 key 原样）
std::string tr(const std::string& key);

}  // namespace i18n
}  // namespace fs
