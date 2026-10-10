// fs_i18n.cpp - embedded zh/en tables
#include "fs_i18n.hpp"

namespace fs {
namespace i18n {

namespace {

struct Entry {
  const char* key;
  const char* zh;
  const char* en;
};

// v1 空表起步（P4 填文案）；key 找不到返回原样
const Entry kZhTable[] = {};
const Entry kEnTable[] = {};
constexpr int kZhCount = sizeof(kZhTable) / sizeof(Entry);
constexpr int kEnCount = sizeof(kEnTable) / sizeof(Entry);

Lang g_lang = Lang::kAuto;

const char* find(const Entry* table, int count, const std::string& key) {
  for (int i = 0; i < count; ++i) {
    if (key == table[i].key) return table[i].zh;
  }
  return nullptr;
}

}  // namespace

void set_override_lang(Lang l) { g_lang = l; }

const char* lang_tag(Lang l) {
  switch (l) {
    case Lang::kZh: return "中文";
    case Lang::kEn: return "English";
    case Lang::kAuto:
    default: return "Auto";
  }
}

Lang effective_lang() {
  if (g_lang != Lang::kAuto) return g_lang;
  return Lang::kEn;  // P4: 跟随系统 locale
}

std::string tr(const std::string& key) {
  Lang l = effective_lang();
  const char* hit = nullptr;
  if (l == Lang::kZh) {
    hit = find(kZhTable, kZhCount, key);
  } else {
    hit = find(kEnTable, kEnCount, key);
  }
  if (hit) return hit;
  return key;
}

}  // namespace i18n
}  // namespace fs