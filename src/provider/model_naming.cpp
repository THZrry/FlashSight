// model_naming.cpp - 模型名粗匹配实现（档位 / 品牌图标）
//
// 只依赖 <string>/<vector>：后端与 wasm UI 都要编译这份（见头文件说明）。
#include "model_naming.hpp"

namespace fs {

namespace {

std::string lower_ascii(const std::string& s) {
  std::string r = s;
  for (size_t i = 0; i < r.size(); ++i) {
    if (r[i] >= 'A' && r[i] <= 'Z') r[i] = static_cast<char>(r[i] - 'A' + 'a');
  }
  return r;
}

bool has(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

bool has_any(const std::string& hay, const char* const* needles, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (has(hay, needles[i])) return true;
  }
  return false;
}

size_t count_of(const char* const* p) {
  size_t n = 0;
  while (p[n]) ++n;
  return n;
}

}  // namespace

std::vector<std::string> thinking_levels_for(const std::string& model_id) {
  const std::string m = lower_ascii(model_id);
  std::vector<std::string> lv;
  // 分级：推理强度可分档
  static const char* const kGraded[] = {"o1", "o3", "o4", "gpt-5", "gpt-6",
                                        "deepseek-v4", "qwen3.6", "qwen3.7",
                                        "qwen3.8", "glm-5", "kimi-k3",
                                        "minimax-m3", "grok-4", nullptr};
  static const char* const kClaude[] = {"claude", "anthropic", nullptr};
  static const char* const kGemini[] = {"gemini-2.5", "gemini-3", "gemini-4",
                                        "gemini-thinking", nullptr};
  // 国产推理模型：能推理但只有开关，没有中间档
  static const char* const kCnReason[] = {"deepseek-r", "deepseek-reasoner", "qwq",
                                          "glm-z1", "kimi-thinking",
                                          "kimi-k2-thinking", "ernie-x1", nullptr};
  // 混合推理（可关可开，档位少）
  static const char* const kToggle[] = {"deepseek-v3.1", "deepseek-v3-1", "qwen3",
                                        "minimax-m", "hunyuan-t1", nullptr};

  if (has_any(m, kGraded, count_of(kGraded)) ||
      has_any(m, kClaude, count_of(kClaude)) ||
      has_any(m, kGemini, count_of(kGemini))) {
    lv.push_back("off");
    lv.push_back("low");
    lv.push_back("medium");
    lv.push_back("high");
    return lv;
  }
  if (has_any(m, kCnReason, count_of(kCnReason))) {
    lv.push_back("off");
    lv.push_back("high");
    return lv;
  }
  if (has_any(m, kToggle, count_of(kToggle))) {
    lv.push_back("off");
    lv.push_back("medium");
    return lv;
  }
  return lv;  // 无档位：UI 不渲染思考控件
}

std::string default_thinking_for(const std::string& model_id) {
  std::vector<std::string> lv = thinking_levels_for(model_id);
  if (lv.empty()) return std::string();
  const std::string m = lower_ascii(model_id);
  // 纯推理模型默认开着（选它就是为了推理）
  static const char* const kAlwaysReason[] = {"deepseek-r", "deepseek-reasoner",
                                              "qwq", "o1", "o3", "o4", "glm-z1",
                                              "ernie-x1", "kimi-k3", nullptr};
  if (has_any(m, kAlwaysReason, count_of(kAlwaysReason))) {
    for (size_t i = 0; i < lv.size(); ++i) {
      if (lv[i] == "high") return "high";
    }
  }
  for (size_t i = 0; i < lv.size(); ++i) {
    if (lv[i] == "medium") return "medium";
  }
  return lv[lv.size() - 1];
}

std::string model_icon_for(const std::string& model_id) {
  const std::string m = lower_ascii(model_id);
  struct Brand {
    const char* icon;
    const char* needles[8];
  };
  // 顺序即优先级：更具体的厂商名排在会被它包含的通用名前面。
  static const Brand kBrands[] = {
      {"openai", {"gpt-", "o1", "o3", "o4", "chatgpt", "davinci", "text-embedding", nullptr}},
      {"claude", {"claude", nullptr}},
      {"anthropic", {"anthropic", nullptr}},
      {"gemini", {"gemini", nullptr}},
      {"gemma", {"gemma", nullptr}},
      {"google", {"palm", nullptr}},
      {"deepseek", {"deepseek", nullptr}},
      {"qwen", {"qwen", "qwq", "tongyi", nullptr}},
      {"alibaba", {"dashscope", nullptr}},
      {"kimi", {"kimi", nullptr}},
      {"moonshot", {"moonshot", nullptr}},
      {"zhipu", {"glm", "zhipu", nullptr}},
      {"chatglm", {"chatglm", nullptr}},
      // "muse" is Meta's model line (Muse Spark); it must win over the bare
      // "spark" below, which means iFlytek's Spark and used to swallow it.
      {"meta", {"llama", "meta-", "muse", nullptr}},
      {"mistral", {"mistral", "mixtral", "codestral", "magistral", nullptr}},
      {"grok", {"grok", nullptr}},
      {"xai", {"xai", nullptr}},
      {"cohere", {"command-r", "cohere", "aya", nullptr}},
      {"perplexity", {"sonar", "perplexity", nullptr}},
      {"yi", {"yi-", "01-ai", nullptr}},
      {"doubao", {"doubao", nullptr}},
      {"bytedance", {"seed-", "skylark", nullptr}},
      {"ernie", {"ernie", "wenxin", nullptr}},
      {"minimax", {"minimax", "abab", nullptr}},
      {"hunyuan", {"hunyuan", nullptr}},
      {"stepfun", {"step-", "stepfun", nullptr}},
      {"spark", {"spark", nullptr}},
      {"iflytek", {"iflytek", nullptr}},
      {"baichuan", {"baichuan", nullptr}},
      {"internlm", {"internlm", "intern-", nullptr}},
      {"nvidia", {"nemotron", "nvidia", nullptr}},
      {"aws", {"nova-", "titan", nullptr}},
      {"bedrock", {"bedrock", nullptr}},
      {"microsoft", {"copilot", nullptr}},
      {"groq", {"groq", nullptr}},
      {"ollama", {"ollama", nullptr}},
      {"lmstudio", {"lmstudio", "lm-studio", nullptr}},
      {"vllm", {"vllm", nullptr}},
      {"opencode", {"opencode", nullptr}},
      {"azure", {"azure", nullptr}},
      {"stability", {"stable-diffusion", nullptr}},
      {"flux", {"flux", nullptr}},
      {"midjourney", {"midjourney", nullptr}},
      {"suno", {"suno", nullptr}},
      {"runway", {"runway", nullptr}},
  };
  for (size_t i = 0; i < sizeof(kBrands) / sizeof(kBrands[0]); ++i) {
    for (size_t k = 0; k < 8; ++k) {
      if (!kBrands[i].needles[k]) break;
      if (has(m, kBrands[i].needles[k])) return kBrands[i].icon;
    }
  }
  return std::string();
}

}  // namespace fs
