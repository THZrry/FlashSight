// model_naming.hpp - 模型名粗匹配（档位 / 品牌图标）
//
// 为什么单独一个模块：这套匹配**两边都要用**。后端在 model.list 里为每个模型
// 补档位和图标，UI 也要能在"用户正在输入一个新模型名"时立刻预览它会有哪些
// 档位。UI 的 wasm 构建不编译 provider 层，所以这张表必须放在一个只依赖
// <string>/<vector> 的地方，两个构建都能直接编进去。
//
// 为什么是"粗匹配"而不是精确表：模型更新太快，任何精确清单立刻过时。真正需要
// 区分的是**家族**（同家族的能力/档位/图标都一样），所以按子串判断家族，认不出
// 来就老实返回空让调用方回退，而不是猜一个错的。
#pragma once

#include <string>
#include <vector>

namespace fs {

// 思考档位。**不是全局固定枚举**——它取决于模型本身：
// o 系列 / gpt-5 分低频到高频，Claude / Gemini 2.5 也分档，
// DeepSeek-R1 这类只有开关，普通对话模型根本没有档位。
// 返回空 = 该模型不可配思考档（UI 因此不渲染控件）。
std::vector<std::string> thinking_levels_for(const std::string& model_id);

// 粗匹配的默认档位（一定在 levels 里；levels 为空时返回空串）
std::string default_thinking_for(const std::string& model_id);

// 品牌图标键，对应 web/icons/lobe/<key>.svg（见 tools/make_lobe_subset.py）。
// 空 = 认不出品牌，UI 回退到通用图标。
std::string model_icon_for(const std::string& model_id);

}  // namespace fs
