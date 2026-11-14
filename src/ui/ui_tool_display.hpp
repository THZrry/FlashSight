// ui_tool_display.hpp - the one-line label of a tool row, generated for HUMANS.
//
// Tool results used to carry a `summary` string that was serialized into the
// model-facing JSON as well, so the model paid for text meant for the
// transcript ("read_file ... 12-40/200 lines"). The model only needs the data;
// the display label is derived here from name + args + result detail.
#pragma once

#include <string>

#include "fs_json.hpp"

namespace fs {
namespace ui {

std::string tool_display_summary(const std::string& name, const Json& args,
                                 const std::string& detail, bool ok);

}  // namespace ui
}  // namespace fs
