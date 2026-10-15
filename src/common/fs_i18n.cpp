// fs_i18n.cpp - 编译内嵌 zh/en 语言表 + 系统 locale 推断
#include "fs_i18n.hpp"

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace fs {
namespace i18n {

namespace {

// 中文是源码语言，也是查表 key；这里只维护 en 侧。
// 缺条目时 tr() 回退到中文原文（可读），不是把 key 泄漏给用户。
struct Entry {
  const char* zh;
  const char* en;
};

const Entry kEnTable[] = {
    // ---- 导航 / 页面 ----
    {"主页", "Home"},
    {"设置", "Settings"},
    {"探索", "Explore"},
    {"项目", "Projects"},
    {"用量统计", "Usage"},
    {"会话", "Session"},
    {"开始新的对话。输入消息后 Enter 发送。",
     "Start a new conversation. Type a message and press Enter to send."},
    {"打开文件夹", "Open folder"},
    {"新建项目", "New project"},
    {"还没有项目。点「打开文件夹」选择目录，或直接把文件夹拖进窗口。",
     "No projects yet. Click \"Open folder\", or drag a folder into the window."},

    // ---- 设置导航 ----
    {"通用", "General"},
    {"模型", "Models"},
    {"权限", "Permissions"},
    {"其他", "Other"},
    {"常规", "General"},
    {"外观", "Appearance"},
    {"供应商", "Providers"},
    {"会话默认", "Session defaults"},
    {"子代理", "Subagents"},
    {"MCP 与 Skill", "MCP & Skills"},
    {"长期记忆", "Long-term memory"},
    {"网络", "Network"},
    {"终端", "Terminal"},
    {"文件", "Files"},
    {"环境", "Environment"},
    {"浏览器控制", "Browser control"},
    {"电脑控制", "Computer control"},
    {"快捷键", "Shortcuts"},
    {"关于", "About"},

    // ---- 设置项 ----
    {"语言", "Language"},
    {"界面语言；「跟随系统」按操作系统 locale 选择",
     "UI language; \"Follow system\" uses the OS locale"},
    {"跟随系统", "Follow system"},
    {"思考语言", "Reasoning language"},
    {"默认 shell", "Default shell"},
    {"编码", "Encoding"},
    {"系统默认", "System default"},
    {"自动探测", "Auto-detect"},
    {"保存", "Save"},
    {"取消", "Cancel"},
    {"删除", "Delete"},
    {"新增", "Add"},
    {"新建", "New"},
    {"编辑", "Edit"},
    {"测试", "Test"},
    {"启用", "Enabled"},
    {"已保存", "Saved"},
    {"保存失败", "Save failed"},

    // ---- 会话 / 输入框 ----
    {"输入消息…（Enter 发送，Shift+Enter 换行）",
     "Type a message… (Enter sends, Shift+Enter adds a line)"},
    {"发送", "Send"},
    {"挂起", "Suspend"},
    {"恢复", "Resume"},
    {"文件", "Files"},
    {"命令", "Commands"},
    {"规划", "Plan"},
    {"已复制", "Copied"},
    {"复制", "Copy"},

    // ---- 终端栏 ----
    {"所有终端", "All terminals"},
    {"终端类型", "Terminal type"},
    {"名称（可选）", "Name (optional)"},
    {"启动", "Start"},
    {"停止", "Stop"},
    {"运行中", "running"},
    {"已退出", "exited"},
    {"无终端", "No terminals"},
    {"暂无输出", "No output yet"},
    {"（暂无输出）", "(no output yet)"},
    {"点「新增」启动一个终端", "Click \"Add\" to start a terminal"},
    {"命令…", "Command…"},
    {"请输入命令", "Enter a command"},
    {"启动失败", "Failed to start"},
    {"发送失败", "Send failed"},
    {"请先打开一个会话", "Open a session first"},
    {"设置页无法插入", "Cannot insert on the settings page"},
    {"拖拽调整终端高度", "Drag to resize the terminal"},
    {"新建终端", "New terminal"},
    {"终端输入…（Enter 发送给终端）", "Terminal input… (Enter sends)"},
    {"终端已退出，无法输入", "Terminal has exited; input disabled"},

    // ---- 拖拽 ----
    {"已插入链接", "Link inserted"},
    {"已插入文件名", "File name inserted"},
    {"未找到文件夹", "Folder not found"},
    {"请用「打开文件夹」手动选择", "use \"Open folder\" to pick it manually"},
    {"已打开拖入的项目", "Opened the dropped project"},
    {"识别为链接", "Detected as a link"},

    // ---- 供应商 ----
    {"新建供应商", "New provider"},
    {"编辑供应商", "Edit provider"},
    {"格式", "Format"},
    {"端点", "Endpoint"},
    {"API 密钥", "API key"},
    {"自定义请求头", "Custom headers"},
    {"发送会话亲和头", "Send session affinity header"},
    {"高级", "Advanced"},
    {"模板", "Templates"},
    {"模型", "Model"},
    {"名称", "Name"},

    // ---- 侧栏 ----
    {"用量", "Usage"},
    {"操作", "Operations"},
    {"调用", "Tool calls"},
    {"面板内容", "Panel"},
    {"添加面板", "Add panel"},
};

const int kEnCount = static_cast<int>(sizeof(kEnTable) / sizeof(kEnTable[0]));

Lang g_lang = Lang::kAuto;

const char* lookup_en(const std::string& key) {
  for (int i = 0; i < kEnCount; ++i) {
    if (key == kEnTable[i].zh) return kEnTable[i].en;
  }
  return nullptr;
}

}  // namespace

void set_lang(Lang l) { g_lang = l; }

// 兼容旧调用点（P4 落地前就存在）
void set_override_lang(Lang l) { set_lang(l); }

const char* lang_tag(Lang l) {
  switch (l) {
    case Lang::kZh: return "中文";
    case Lang::kEn: return "English";
    case Lang::kAuto:
    default: return "跟随系统";
  }
}

Lang lang_from_setting(const std::string& v) {
  if (v == "zh") return Lang::kZh;
  if (v == "en") return Lang::kEn;
  return Lang::kAuto;
}

const char* setting_from_lang(Lang l) {
  switch (l) {
    case Lang::kZh: return "zh";
    case Lang::kEn: return "en";
    case Lang::kAuto:
    default: return "auto";
  }
}

Lang system_lang() {
#if defined(_WIN32)
  // 0x04 = 中文（zh-Hans / zh-Hant 同属 primary language 4）
  LANGID id = GetUserDefaultUILanguage();
  if ((id & 0x3FF) == 0x04) return Lang::kZh;
  return Lang::kEn;
#elif defined(__EMSCRIPTEN__)
  // navigator.language，如 "zh-CN" / "en-US"
  int zh = EM_ASM_INT({
    var l = (navigator.language || "en").toLowerCase();
    return l.indexOf("zh") === 0 ? 1 : 0;
  });
  return zh ? Lang::kZh : Lang::kEn;
#else
  const char* lc = std::getenv("LANG");
  if (lc && std::string(lc).find("zh") != std::string::npos) return Lang::kZh;
  return Lang::kEn;
#endif
}

Lang effective_lang() {
  if (g_lang != Lang::kAuto) return g_lang;
  return system_lang();
}

std::string tr(const std::string& key) {
  if (effective_lang() == Lang::kZh) return key;  // 中文即原文
  const char* hit = lookup_en(key);
  return hit ? std::string(hit) : key;
}

}  // namespace i18n
}  // namespace fs
