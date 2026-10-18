// fs_highlight.hpp - lightweight syntax highlighting for code DISPLAY.
//
// Scope, deliberately: this is a highlighter, not a parser. It never validates
// code and never tries to be exact - a mis-coloured identifier is not a bug, a
// crash or a hang is. That lets it work on half-written code, unknown dialects,
// and files whose language we only guessed.
//
// Why not re2c (or a generated DFA per language):
//   * A re2c lexer for C++ is ~60-120 DFA states, each a 256-entry table:
//     30-60 KB of tables per language. For "several dozen languages" that is
//     2-4 MB of binary - for a feature that only draws colours.
//   * Every language needs its own .re source kept in sync, regenerated at build
//     time, and a build rule that produces C++ from it.
// This design is one hand-written scanner whose behaviour is DATA: a LangSpec
// (comment markers, string forms, case sensitivity, ...) plus a keyword list.
// Adding a language is one row in a table, no code generation and no new states.
// 45 languages fit in ~40 KB of source, and the scanner is shared by all.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fs {
namespace highlight {

// Token classes the renderer colours. Plain text is a span too (see tokenize())
// so that concatenating spans always reproduces the input.
enum class Kind {
  kPlain = 0,
  kKeyword,
  kType,
  kString,
  kNumber,
  kComment,
  kPreproc,   // #include / #define; @annotation
  kFunction,  // identifier followed by '('
  kAttr,      // HTML attribute / CSS property / JSON key
  kTag,       // HTML/XML tag name, CSS selector
  kMeta,      // markdown markers, diff headers
  kAdded,     // diff '+' lines
  kRemoved,   // diff '-' lines
};

// Which scanner drives this language. kGeneric covers everything that is
// "identifiers, numbers, strings, comments"; the others need their own shape.
enum class Syntax {
  kGeneric = 0,
  kMarkup,   // HTML / XML / Vue / Svelte
  kCss,      // CSS / SCSS / LESS
  kJson,
  kMarkdown,
  kDiff,
};

// Behaviour switches for the generic scanner. Bit flags rather than a dozen
// bools: a table of 45 rows is unreadable (and easy to mis-count) otherwise.
enum Flag : uint32_t {
  kBlockComments = 1u << 0,   // /* ... */
  kNestedComments = 1u << 1,  // Rust/Swift-style nesting
  kCPreproc = 1u << 2,        // a line starting with '#' is a directive
  kCaseInsensitive = 1u << 3, // SQL/VB-style keywords
  kTripleStrings = 1u << 4,   // """...""" and '''...'''
  kRawStrings = 1u << 5,      // C++ R"(..)", Rust r#".."#, C# @".."
  kDollarIdents = 1u << 6,    // PHP/Shell $var
  kAtPreproc = 1u << 7,       // Java/C# annotations (@Override)
  kBacktickStrings = 1u << 8, // JS/Go template strings, Shell `cmd`
  kHashAttrs = 1u << 9,       // Rust #[derive(...)] / #[serde(...)]
};

struct Span {
  Kind kind;
  std::string text;  // exact source text (not escaped)
};

// One language. `keywords` / `types` are space-separated word lists: one string
// literal per language keeps the table compact and readable, and the lookup
// sets are built lazily on first use (no startup cost for 45 languages).
struct LangSpec {
  const char* id;           // "cpp", "python", ... (lowercase, matches fences)
  Syntax syntax;
  const char* keywords;     // control flow / declarations
  const char* types;        // built-in types / well-known classes
  const char* line_comments;  // markers separated by '|' ("//", "#", "--")
  uint32_t flags;           // Flag bits
};

// The table of supported languages. Never null; unknown ids fall back to plain.
const LangSpec* find_lang(const std::string& id);
// Guess a language from a file name ("a.cpp" -> "cpp", "Dockerfile" -> "docker").
std::string lang_for_path(const std::string& path);
// All ids, for tests and diagnostics.
std::vector<std::string> all_langs();

// Split `code` into spans. Concatenating every span's text reproduces `code`
// exactly - the renderer relies on that to build one HTML string.
std::vector<Span> tokenize(const std::string& code, const std::string& lang);

// HTML for one code block: each span becomes <span class="hl-kw">escaped</span>
// and plain gaps are escaped verbatim. This is ONE string, not a DOM subtree:
// a 200-line block would otherwise create thousands of nodes, and the UI
// re-renders several times a second while a turn streams.
std::string to_html(const std::string& code, const std::string& lang);
// Same, cached by (lang, content hash). The UI calls this on every render, so
// without a cache the same block would be tokenized forever.
const std::string& to_html_cached(const std::string& code, const std::string& lang);

// Escape text for HTML (& < > " ').
std::string escape_html(const std::string& s);

}  // namespace highlight
}  // namespace fs
