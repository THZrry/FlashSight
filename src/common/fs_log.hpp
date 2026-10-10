// fs_log.hpp - leveled logging stub
// 约定（arch.md §6）：终端输出与模型流式输出走事件，不走日志；日志分级 debug/info/warn/error
#pragma once

#include <string>

namespace fs {
namespace log {

enum class Level { kDebug, kInfo, kWarn, kError };

// 全局级别，默认 info；debug 级只在 debug 构建开
void set_level(Level lv);
bool enabled(Level lv);

void debug(const std::string& msg);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

}  // namespace log
}  // namespace fs