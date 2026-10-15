// fs_encoding.hpp - subprocess output -> UTF-8 (terminal.md §5 编码)
//
// 子进程的输出是字节流，不是文本：cmd.exe 在中文 Windows 上按 OEM 代码页
// （GBK）输出，PowerShell 可能是 UTF-16LE 或 GBK。这些字节直接塞进 JSON 会
// 让 dump() 抛 type_error.316（非法 UTF-8），HTTP 层就变成 500。
// 所以所有"进程输出 -> UI/模型"的路径都必须先经过这里转成合法 UTF-8。
#pragma once

#include <string>

namespace fs {
namespace encoding {

// 把一段原始输出字节转成合法 UTF-8。
//   enc: ""（系统默认：Windows 用 OEM 代码页）| "UTF-8" | "GBK"
// 无法解码的字节变成 U+FFFD；末尾不完整的多字节序列留在 *pending，等下一块
// 数据补齐（跨 read 边界的汉字不会被切成两个替换符）。
std::string decode_output(const std::string& bytes, const std::string& enc,
                          std::string* pending);

// 只做合法性处理：非法序列 -> U+FFFD。用于已经是 UTF-8（或来源未知）的文本。
std::string sanitize_utf8(const std::string& bytes);

// UTF-8 文本 -> 指定代码页字节（写进程 stdin 用：cmd.exe 按 OEM 代码页读管道）。
// cp 为 0 时不做转换，原样返回。
std::string utf8_to_codepage(const std::string& text, unsigned cp);

// UTF-8 文本 -> 指定代码页（名字形式："UTF-8" 原样 / "GBK" / 其它走系统默认）
std::string encode_input(const std::string& text, const std::string& enc);

}  // namespace encoding
}  // namespace fs
