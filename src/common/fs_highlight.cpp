// fs_highlight.cpp - one scanner, data-driven per language.
#include "fs_highlight.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_set>

namespace fs {
namespace highlight {
namespace {

// ---------------------------------------------------------------------------
// Language table
// ---------------------------------------------------------------------------
// Word lists are space separated. Keeping them as one literal per language is
// the whole point of the design: adding a language is one row, and the binary
// cost is the words themselves (a few hundred bytes), not a state machine.
//
// The lists are deliberately SHORT: a highlighter for reading code does not
// need every member of the STL. The most common 20-40 words carry almost all of
// the visual signal; a 400-word list would cost space and show no difference.

#define G(id, kw, ty, lc, fl) {id, Syntax::kGeneric, kw, ty, lc, fl}

// The four shapes that cover most languages. Written out so a row says what it
// MEANS rather than a number nobody can decode.
//   CFAM  - C family: /* */, //, #directive
//   CLANG - C-like without a preprocessor but with annotations (@Override)
//   SCRIPT- script language: # comments, no block comments
//   DATA  - data/config: # comments, no keywords worth colouring
#define CFAM(id, kw, ty) \
  G(id, kw, ty, "//", kBlockComments | kCPreproc)
#define CFAM_AT(id, kw, ty) \
  G(id, kw, ty, "//", kBlockComments | kAtPreproc)
#define CLANG(id, kw, ty) G(id, kw, ty, "//", kBlockComments)
#define SCRIPT(id, kw, ty) G(id, kw, ty, "#", 0u)

const LangSpec kLangs[] = {
    // ---- C family (one template, several ids) ----
    CFAM("c",
         "if else for while do switch case default break continue return goto sizeof typedef struct union enum extern static const volatile inline register",
         "void char short int long float double signed unsigned size_t bool"),
    G("cpp",
      "if else for while do switch case default break continue return goto sizeof typedef struct union enum class namespace template typename using public private protected virtual override final constexpr consteval noexcept static_cast dynamic_cast reinterpret_cast const_cast new delete this friend operator explicit mutable inline const volatile extern static thread_local co_await co_return co_yield concept requires",
      "void char short int long float double signed unsigned bool size_t wchar_t char8_t char16_t char32_t auto std string vector map set unique_ptr shared_ptr optional variant",
      "//", kBlockComments | kCPreproc | kRawStrings),
    G("csharp",
      "if else for foreach while do switch case default break continue return goto class struct interface enum namespace using public private protected internal virtual override abstract sealed static readonly const new delete this base operator async await try catch finally throw is as typeof nameof lock yield record",
      "void int uint long ulong short ushort byte sbyte float double decimal bool char string object var dynamic Task List Dictionary",
      "//", kBlockComments | kAtPreproc | kRawStrings),
    CFAM_AT("java",
            "if else for while do switch case default break continue return class interface enum extends implements public private protected static final abstract synchronized volatile transient native new this super try catch finally throw throws instanceof import package record var",
            "void int long short byte char float double boolean String Object List Map Set Integer Double Boolean"),
    G("kotlin",
      "if else for while do when break continue return class object interface fun val var is as in out by companion data sealed override open abstract private public protected internal suspend inline lateinit init constructor try catch finally throw import package",
      "Unit Int Long Short Byte Char Float Double Boolean String Any List MutableList Map MutableMap Set",
      "//", kBlockComments | kAtPreproc | kTripleStrings),
    G("swift",
      "if else for while repeat switch case default break continue return func class struct enum protocol extension import guard defer throw throws do catch try as is in where let var init deinit override final open public private fileprivate internal static lazy weak unowned some any async await actor",
      "Void Int Double Float Bool String Character Array Dictionary Set Optional Result Error",
      "//", kBlockComments | kNestedComments | kAtPreproc | kTripleStrings),
    G("scala",
      "if else for while match case class object trait extends with val var def override private protected final abstract sealed implicit lazy import package new try catch finally throw yield forSome",
      "Unit Int Long Double Float Boolean Char String Any AnyRef List Seq Map Set Option Either Future",
      "//", kBlockComments | kNestedComments | kAtPreproc | kTripleStrings),
    G("rust",
      "if else for while loop match break continue return fn struct enum trait impl use mod pub crate self super as in where let mut ref move async await dyn unsafe extern type const static",
      "i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box Rc Arc HashMap",
      "//", kBlockComments | kNestedComments | kRawStrings | kHashAttrs),
    CFAM("go",
         "if else for range switch case default break continue return func type struct interface map chan go defer select package import var const",
         "int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 uintptr float32 float64 complex64 complex128 byte rune string bool error any"),
    G("dart",
      "if else for while do switch case default break continue return class extends implements mixin enum abstract final const var late static new this super try catch finally throw async await yield import export library part typedef",
      "void int double num bool String List Map Set Future Stream dynamic Object",
      "//", kBlockComments | kAtPreproc),
    G("php",
      "if else elseif for foreach while do switch case default break continue return function class interface trait extends implements public private protected static final abstract new clone echo print require include namespace use try catch finally throw instanceof global isset unset empty",
      "int float string bool array callable iterable object mixed void null",
      "//|#", kBlockComments | kDollarIdents),
    G("javascript",
      "if else for while do switch case default break continue return function class extends implements import export from as const let var new delete typeof instanceof in of this super try catch finally throw async await yield void null undefined true false",
      "Number String Boolean Object Array Map Set Promise Symbol BigInt Error JSON Math Date RegExp",
      "//", kBlockComments | kBacktickStrings),
    G("typescript",
      "if else for while do switch case default break continue return function class extends implements interface import export from as const let var new delete typeof instanceof in of this super try catch finally throw async await yield void null undefined true false type enum namespace declare readonly public private protected abstract satisfies keyof infer",
      "number string boolean object any unknown never void Array Map Set Promise Record Partial Readonly Pick Omit",
      "//", kBlockComments | kBacktickStrings | kAtPreproc),
    G("jsx",
      "if else for while do switch case default break continue return function class extends import export from const let var new delete typeof this super try catch finally throw async await yield",
      "Number String Boolean Object Array Map Set Promise",
      "//", kBlockComments | kBacktickStrings),
    G("tsx",
      "if else for while do switch case default break continue return function class extends implements interface import export from as const let var new delete typeof this super try catch finally throw async await yield type enum namespace public private protected readonly",
      "number string boolean object any unknown never void Array Map Set Promise Record",
      "//", kBlockComments | kBacktickStrings),
    CFAM("objectivec",
         "if else for while do switch case default break continue return interface implementation property end class protocol selector synthesize dynamic",
         "void int long float double BOOL id NSObject NSString NSArray NSDictionary"),
    G("groovy",
      "if else for while switch case default break continue return def class interface enum extends implements public private protected static final new this super try catch finally throw import package",
      "int long double float boolean char String Object List Map Set",
      "//", kBlockComments | kTripleStrings),
    CLANG("vala",
          "if else for while do switch case default break continue return class struct enum interface namespace using public private protected internal static const new this base try catch finally throw",
          "void int long short char float double bool string Object"),
    CFAM("zig",
         "if else for while switch break continue return fn struct enum union const var pub usingnamespace comptime inline export extern async await defer errdefer try catch orelse",
         "void bool u8 u16 u32 u64 usize i8 i16 i32 i64 isize f16 f32 f64 f128 anytype"),
    CFAM("solidity",
         "if else for while do switch case default break continue return contract interface library function modifier event struct enum mapping public private internal external pure view payable memory storage calldata returns emit require revert import pragma",
         "address bool string bytes uint int uint8 uint256 bytes32"),
    CFAM("glsl",
         "if else for while do switch case default break continue return precision highp mediump lowp in out inout uniform varying attribute const void struct",
         "float int uint bool vec2 vec3 vec4 ivec2 ivec3 ivec4 mat2 mat3 mat4 sampler2D samplerCube"),
    CFAM("cuda",
         "if else for while do switch case default break continue return global device host shared constant launch_bounds syncthreads",
         "void char short int long float double bool dim3 size_t"),

    // ---- C-like without a preprocessor ----
    CLANG("actionscript",
          "if else for while do switch case default break continue return function class interface extends implements public private protected internal static final new this super try catch finally throw import package var const",
          "void int uint Number String Boolean Object Array"),
    CLANG("haxe",
          "if else for while do switch case default break continue return class interface enum typedef extends implements public private static final new this super try catch throw import package var function",
          "Void Int Float Bool String Array Map Dynamic"),

    // ---- Python family ----
    G("python",
      "if elif else for while break continue return def class import from as pass raise try except finally with yield lambda global nonlocal assert del async await in is not and or None True False",
      "int float complex bool str bytes list dict set tuple frozenset object type Any Optional List Dict Set Tuple",
      "#", kTripleStrings),
    G("ruby",
      "if elsif else unless end for while until break next redo retry return def class module self nil true false and or not yield begin rescue ensure raise case when then do require include extend attr_accessor attr_reader",
      "Integer Float String Symbol Array Hash Range Proc Lambda Struct",
      "#", 0u),
    SCRIPT("perl",
           "if elsif else unless while until for foreach break next last redo return sub package use require my our local state print say die warn eval",
           "int float double string array hash scalar"),
    SCRIPT("lua",
           "if elseif else then end for while repeat until do break return function local and or not nil true false goto in",
           "string number table boolean function thread userdata"),
    SCRIPT("r",
           "if else repeat while function for in next break return library require",
           "numeric character logical integer double complex list data.frame matrix vector factor"),
    SCRIPT("julia",
           "if elseif else for while begin end break continue return function macro module using import export struct mutable where do let try catch finally throw",
           "Int Float64 Float32 String Char Bool Vector Matrix Array Tuple Dict Any"),
    G("elixir",
      "if unless case cond for with do end def defp defmodule defstruct fn when and or not in raise rescue after catch else try receive import require use alias",
      "Integer Float String Atom List Map Tuple",
      "#", kTripleStrings),
    SCRIPT("erlang",
           "if case of end fun when receive after try catch throw and or not begin let",
           "integer float atom list tuple binary pid"),
    SCRIPT("nim",
           "if elif else for while case of break continue return proc func method iterator template macro type var let const import export from as discard try except finally raise yield",
           "int int8 int16 int32 int64 uint float float32 float64 bool char string seq array"),
    SCRIPT("crystal",
           "if elsif else unless case when then end while until for break next return def class module struct enum union self nil true false and or not yield begin rescue ensure raise require include extend",
           "Int32 Int64 Float64 String Bool Char Array Hash Symbol"),
    CLANG("d",
          "if else for foreach while do switch case default break continue return class struct union enum interface import module public private protected static final const immutable auto ref out in try catch finally throw scope with",
          "void bool byte ubyte short ushort int uint long ulong float double real char wchar string"),

    // ---- shells ----
    G("bash",
      "if then elif else fi for in do done while until case esac function select return break continue local export readonly declare typeset unset shift source alias",
      "", "#", kDollarIdents | kBacktickStrings),
    G("powershell",
      "if elseif else switch foreach for while do until break continue return function filter param begin process end try catch finally throw trap in",
      "int long string bool array hashtable psobject",
      "#", kDollarIdents),
    G("batch",
      "if else for in do goto call setlocal endlocal exit echo rem shift",
      "", "::", 0u),

    // ---- data / config (comments only; keywords are too generic to colour) ----
    SCRIPT("yaml", "", ""),
    SCRIPT("toml", "", ""),
    G("ini", "", "", "#|;", 0u),
    SCRIPT("makefile",
           "ifeq ifneq ifdef ifndef else endif include define endef export unexport override", ""),
    G("cmake",
      "if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro set unset list string message project add_executable add_library target_link_libraries target_include_directories include find_package install option return",
      "", "#", kCaseInsensitive),
    G("docker",
      "FROM RUN CMD LABEL EXPOSE ENV ADD COPY ENTRYPOINT VOLUME USER WORKDIR ARG ONBUILD STOPSIGNAL HEALTHCHECK SHELL",
      "", "#", kCaseInsensitive),
    G("sql",
      "select from where group by having order limit offset insert into values update set delete create table alter drop index view join left right inner outer full on as and or not null is in like between distinct union all case when then else end exists",
      "int bigint smallint varchar char text date datetime timestamp boolean decimal numeric float double blob",
      "--", kBlockComments),
    G("sql_ci",
      "select from where group by having order limit offset insert into values update set delete create table alter drop index view join left right inner outer full on as and or not null is in like between distinct union all case when then else end exists",
      "int bigint smallint varchar char text date datetime timestamp boolean decimal numeric float double blob",
      "--", kBlockComments | kCaseInsensitive),
    SCRIPT("graphql",
           "query mutation subscription fragment on type interface union enum input scalar extend schema directive",
           "String Int Float Boolean ID"),
    SCRIPT("protobuf",
           "syntax package import option message enum service rpc returns repeated optional required oneof reserved extend map",
           "double float int32 int64 uint32 uint64 sint32 sint64 fixed32 fixed64 bool string bytes"),
    SCRIPT("terraform",
           "resource data variable output module provider locals terraform for_each count depends_on",
           "string number bool list map object any"),
    SCRIPT("hcl",
           "resource data variable output module provider locals terraform for_each count",
           "string number bool list map object"),

    // ---- dedicated scanners ----
    {"html", Syntax::kMarkup, "", "", "", 0u},
    {"xml", Syntax::kMarkup, "", "", "", 0u},
    {"vue", Syntax::kMarkup, "", "", "", 0u},
    {"svelte", Syntax::kMarkup, "", "", "", 0u},
    {"css", Syntax::kCss, "", "", "", 0u},
    {"scss", Syntax::kCss, "", "", "//", 0u},
    {"less", Syntax::kCss, "", "", "//", 0u},
    {"json", Syntax::kJson, "", "", "", 0u},
    {"jsonc", Syntax::kJson, "", "", "//", kBlockComments},
    {"markdown", Syntax::kMarkdown, "", "", "", 0u},
    {"diff", Syntax::kDiff, "", "", "", 0u},
};

const size_t kLangCount = sizeof(kLangs) / sizeof(kLangs[0]);

// Alias table: the ids people actually write in fences.
struct Alias {
  const char* from;
  const char* to;
};
const Alias kAliases[] = {
    {"c++", "cpp"},        {"cc", "cpp"},          {"cxx", "cpp"},
    {"h", "cpp"},          {"hpp", "cpp"},         {"hxx", "cpp"},
    {"cs", "csharp"},      {"c#", "csharp"},       {"kt", "kotlin"},
    {"kts", "kotlin"},     {"m", "objectivec"},    {"mm", "objectivec"},
    {"objc", "objectivec"},{"js", "javascript"},   {"mjs", "javascript"},
    {"cjs", "javascript"}, {"jsx", "jsx"},         {"ts", "typescript"},
    {"mts", "typescript"}, {"tsx", "tsx"},         {"py", "python"},
    {"py3", "python"},     {"python3", "python"},  {"rb", "ruby"},
    {"rs", "rust"},        {"golang", "go"},       {"sh", "bash"},
    {"shell", "bash"},     {"zsh", "bash"},        {"console", "bash"},
    {"ps1", "powershell"}, {"pwsh", "powershell"}, {"ps", "powershell"},
    {"bat", "batch"},      {"cmd", "batch"},       {"yml", "yaml"},
    {"md", "markdown"},    {"patch", "diff"},      {"htm", "html"},
    {"xhtml", "html"},     {"svg", "xml"},         {"plist", "xml"},
    {"scss2", "scss"},     {"postcss", "css"},     {"mysql", "sql_ci"},
    {"postgres", "sql_ci"},{"postgresql", "sql_ci"},{"plsql", "sql_ci"},
    {"tf", "terraform"},   {"tfvars", "terraform"},{"proto", "protobuf"},
    {"gql", "graphql"},    {"dockerfile", "docker"},{"ex", "elixir"},
    {"exs", "elixir"},     {"jl", "julia"},        {"raku", "perl"},
    {"text", "plain"},     {"txt", "plain"},       {"plaintext", "plain"},
    {"none", "plain"},     {"", "plain"},
};

const LangSpec kPlainSpec = {"plain", Syntax::kGeneric, "", "", "", 0u};

// ---------------------------------------------------------------------------
// Keyword lookup
// ---------------------------------------------------------------------------
// One set per language, built on first use. A language nobody opens costs
// nothing; the whole table is ~1000 words so building them all is still cheap.
struct WordSet {
  std::unordered_set<std::string> words;
  bool ready = false;
};

std::string lower_ascii(const std::string& s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
  }
  return out;
}

void fill_words(WordSet* ws, const char* list) {
  if (ws->ready) return;
  ws->ready = true;
  if (!list || !*list) return;
  std::string cur;
  for (const char* p = list;; ++p) {
    char c = *p;
    if (c == ' ' || c == '\0') {
      if (!cur.empty()) {
        ws->words.insert(cur);
        cur.clear();
      }
      if (c == '\0') break;
    } else {
      cur.push_back(c);
    }
  }
}

// Indexed by language position in kLangs, so a lookup is one array access.
WordSet g_keywords[kLangCount];
WordSet g_types[kLangCount];

size_t index_of(const LangSpec* spec) {
  if (!spec) return kLangCount;
  ptrdiff_t d = spec - kLangs;
  if (d < 0 || d >= static_cast<ptrdiff_t>(kLangCount)) return kLangCount;
  return static_cast<size_t>(d);
}

bool in_words(size_t lang_index, const std::string& word, bool types) {
  if (lang_index >= kLangCount) return false;
  const LangSpec& spec = kLangs[lang_index];
  WordSet& ws = types ? g_types[lang_index] : g_keywords[lang_index];
  fill_words(&ws, types ? spec.types : spec.keywords);
  std::string key = (spec.flags & kCaseInsensitive) ? lower_ascii(word) : word;
  return ws.words.count(key) != 0;
}

// ---------------------------------------------------------------------------
// Scanner helpers
// ---------------------------------------------------------------------------

bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}
bool is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void push(std::vector<Span>* out, Kind k, const std::string& text) {
  if (text.empty()) return;
  // Merge with the previous span when the class is the same: a string split by
  // an escape sequence is still one string to the reader (and fewer spans).
  if (!out->empty() && out->back().kind == k) {
    out->back().text += text;
    return;
  }
  Span s;
  s.kind = k;
  s.text = text;
  out->push_back(s);
}

bool starts_with_at(const std::string& s, size_t i, const char* lit) {
  size_t n = std::strlen(lit);
  return i + n <= s.size() && s.compare(i, n, lit) == 0;
}

// Line-comment markers of a spec as a small vector (parsed on each call: there
// are at most two markers and this is not the hot path).
std::vector<std::string> line_markers(const LangSpec& spec) {
  std::vector<std::string> out;
  if (!spec.line_comments || !*spec.line_comments) return out;
  std::string cur;
  for (const char* p = spec.line_comments;; ++p) {
    char c = *p;
    if (c == '|' || c == '\0') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      if (c == '\0') break;
    } else {
      cur.push_back(c);
    }
  }
  return out;
}

// A number literal: digits, hex/bin/oct prefixes, separators, exponents and
// (loosely) unit suffixes. Loose on purpose - the worst case is that a suffix is
// coloured as a number, which nobody notices.
size_t scan_number(const std::string& s, size_t i) {
  size_t j = i;
  if (s[j] == '0' && j + 1 < s.size() &&
      (s[j + 1] == 'x' || s[j + 1] == 'X' || s[j + 1] == 'b' || s[j + 1] == 'B' ||
       s[j + 1] == 'o' || s[j + 1] == 'O')) {
    j += 2;
    while (j < s.size() && (is_ident_char(s[j]) || s[j] == '\'')) ++j;
    return j;
  }
  while (j < s.size() && (is_digit(s[j]) || s[j] == '_' || s[j] == '\'')) ++j;
  if (j < s.size() && s[j] == '.') {
    ++j;
    while (j < s.size() && (is_digit(s[j]) || s[j] == '_')) ++j;
  }
  if (j < s.size() && (s[j] == 'e' || s[j] == 'E' || s[j] == 'p' || s[j] == 'P')) {
    size_t k = j + 1;
    if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
    if (k < s.size() && is_digit(s[k])) {
      j = k;
      while (j < s.size() && is_digit(s[j])) ++j;
    }
  }
  // suffix (f, u, l, ll, i32, ...): letters right after the digits
  while (j < s.size() && (is_ident_char(s[j]) || s[j] == '\'')) ++j;
  return j;
}

// A quoted string starting at `i` (s[i] is the quote). Handles backslash escapes
// and, for triple-quoted forms, the longer delimiter. Returns the end index.
size_t scan_quoted(const std::string& s, size_t i, char quote, bool triple) {
  size_t j = i + 1;
  if (triple) {
    if (i + 2 >= s.size() || s[i + 1] != quote || s[i + 2] != quote) {
      // not actually a triple quote: fall back to a normal one
      triple = false;
    } else {
      j = i + 3;
    }
  }
  while (j < s.size()) {
    char c = s[j];
    if (c == '\\') {
      j += 2;
      continue;
    }
    if (triple) {
      if (c == quote && j + 2 < s.size() && s[j + 1] == quote && s[j + 2] == quote) {
        return j + 3;
      }
      ++j;
      continue;
    }
    if (c == quote) return j + 1;
    // An unterminated single-line string stops at the newline: otherwise one
    // stray apostrophe in a comment would paint the rest of the file as a
    // string. (Multi-line backtick/triple forms are handled above.)
    if (c == '\n' && quote != '`') return j;
    ++j;
  }
  return j;
}

// C++ raw string: R"delim( ... )delim"
size_t scan_raw_cpp(const std::string& s, size_t i) {
  // s[i] == 'R', s[i+1] == '"'
  size_t j = i + 2;
  std::string delim;
  while (j < s.size() && s[j] != '(' && s[j] != '\n') {
    delim.push_back(s[j]);
    ++j;
  }
  if (j >= s.size() || s[j] != '(') return i + 1;
  ++j;
  std::string close = ")" + delim + "\"";
  size_t e = s.find(close, j);
  if (e == std::string::npos) return s.size();
  return e + close.size();
}

// Rust raw string: r"..." / r#"..."# / r##"..."##
size_t scan_raw_rust(const std::string& s, size_t i) {
  size_t j = i + 1;  // after 'r'
  size_t hashes = 0;
  while (j < s.size() && s[j] == '#') {
    ++hashes;
    ++j;
  }
  if (j >= s.size() || s[j] != '"') return i + 1;
  ++j;
  std::string close = "\"";
  for (size_t k = 0; k < hashes; ++k) close += "#";
  size_t e = s.find(close, j);
  if (e == std::string::npos) return s.size();
  return e + close.size();
}

// ---------------------------------------------------------------------------
// Generic scanner
// ---------------------------------------------------------------------------
std::vector<Span> tokenize_generic(const std::string& s, const LangSpec& spec,
                                   size_t lang_index) {
  std::vector<Span> out;
  std::vector<std::string> markers = line_markers(spec);
  size_t i = 0;
  size_t n = s.size();
  bool line_start = true;

  auto plain_to = [&out, &s](size_t from, size_t to) {
    if (to > from) push(&out, Kind::kPlain, s.substr(from, to - from));
  };

  while (i < n) {
    char c = s[i];

    // --- line comment ---
    bool matched_marker = false;
    for (size_t m = 0; m < markers.size(); ++m) {
      const std::string& mk = markers[m];
      if (mk.empty()) continue;
      if (starts_with_at(s, i, mk.c_str())) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = n;
        push(&out, Kind::kComment, s.substr(i, e - i));
        i = e;
        matched_marker = true;
        break;
      }
    }
    if (matched_marker) continue;

    // --- C preprocessor: a '#' as the first non-space character of a line ---
    if ((spec.flags & kCPreproc) && c == '#' && line_start) {
      size_t e = s.find('\n', i);
      if (e == std::string::npos) e = n;
      // "#include <x>" - the header is part of the directive visually
      push(&out, Kind::kPreproc, s.substr(i, e - i));
      i = e;
      continue;
    }
    // --- annotations (@Override / @Inject) ---
    if ((spec.flags & kAtPreproc) && c == '@' && i + 1 < n && is_ident_start(s[i + 1])) {
      size_t j = i + 1;
      while (j < n && is_ident_char(s[j])) ++j;
      push(&out, Kind::kPreproc, s.substr(i, j - i));
      i = j;
      continue;
    }
    // --- Rust attributes: #[derive(Debug)] / #[serde(default)] ---
    // Same visual role as a C directive (it decorates the item below it), so it
    // shares the preprocessor colour. Without this, `#[` reads as two stray
    // symbols and `derive` as a function call.
    if (c == '#' && i + 1 < n && s[i + 1] == '[' && (spec.flags & kHashAttrs)) {
      size_t j = i + 2;
      int depth = 1;
      while (j < n && depth > 0) {
        if (s[j] == '[') ++depth;
        else if (s[j] == ']') --depth;
        ++j;
      }
      push(&out, Kind::kPreproc, s.substr(i, j - i));
      i = j;
      continue;
    }

    // --- block comment ---
    if ((spec.flags & kBlockComments) && c == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t j = i + 2;
      int depth = 1;
      while (j < n) {
        if ((spec.flags & kNestedComments) && j + 1 < n && s[j] == '/' && s[j + 1] == '*') {
          ++depth;
          j += 2;
          continue;
        }
        if (j + 1 < n && s[j] == '*' && s[j + 1] == '/') {
          --depth;
          j += 2;
          if (depth == 0) break;
          continue;
        }
        ++j;
      }
      push(&out, Kind::kComment, s.substr(i, j - i));
      i = j;
      continue;
    }

    // --- raw strings (C++ / Rust / C#) ---
    if ((spec.flags & kRawStrings)) {
      if (c == 'R' && i + 1 < n && s[i + 1] == '"') {
        size_t e = scan_raw_cpp(s, i);
        push(&out, Kind::kString, s.substr(i, e - i));
        i = e;
        continue;
      }
      if (c == 'r' && i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '#')) {
        size_t e = scan_raw_rust(s, i);
        push(&out, Kind::kString, s.substr(i, e - i));
        i = e;
        continue;
      }
      if (c == '@' && i + 1 < n && s[i + 1] == '"') {
        // C# verbatim string: "" is an escaped quote, newlines allowed
        size_t j = i + 2;
        while (j < n) {
          if (s[j] == '"') {
            if (j + 1 < n && s[j + 1] == '"') {
              j += 2;
              continue;
            }
            ++j;
            break;
          }
          ++j;
        }
        push(&out, Kind::kString, s.substr(i, j - i));
        i = j;
        continue;
      }
    }

    // --- triple-quoted strings (Python / Kotlin / Swift) ---
    if ((spec.flags & kTripleStrings) && (c == '"' || c == '\'') && i + 2 < n &&
        s[i + 1] == c && s[i + 2] == c) {
      size_t e = scan_quoted(s, i, c, true);
      push(&out, Kind::kString, s.substr(i, e - i));
      i = e;
      continue;
    }

    // --- quoted strings / char literals ---
    if (c == '"' || c == '\'') {
      // A single quote is a char literal in C-family code and a string quote in
      // Python/Ruby. Either way it cannot span lines: scanning it as a string
      // made one lone apostrophe paint the rest of the file, so both stop at a
      // newline. The one exception is a Rust lifetime (`'a`, `'static`), which
      // is an identifier, not a quote - if there is no closing quote on the same
      // line, let the identifier branch handle it.
      bool lifetime = false;
      if (c == '\'') {
        size_t close = i + 1;
        while (close < n && s[close] != '\'' && s[close] != '\n') ++close;
        if (close >= n || s[close] == '\n') lifetime = true;
      }
      if (!lifetime) {
        size_t e = scan_quoted(s, i, c, false);
        push(&out, Kind::kString, s.substr(i, e - i));
        i = e;
        continue;
      }
    }
    if ((spec.flags & kBacktickStrings) && c == '`') {
      size_t e = scan_quoted(s, i, '`', false);
      push(&out, Kind::kString, s.substr(i, e - i));
      i = e;
      continue;
    }

    // --- numbers ---
    if (is_digit(c) ||
        (c == '.' && i + 1 < n && is_digit(s[i + 1]) &&
         (i == 0 || !is_ident_char(s[i - 1])))) {
      size_t e = scan_number(s, i);
      push(&out, Kind::kNumber, s.substr(i, e - i));
      i = e;
      continue;
    }

    // --- identifiers / keywords ---
    if (is_ident_start(c) || ((spec.flags & kDollarIdents) && c == '$')) {
      size_t j = i;
      if (c == '$') ++j;
      while (j < n && is_ident_char(s[j])) ++j;
      std::string word = s.substr(i, j - i);
      // A call: the next non-space character is '(' and the word is not a
      // keyword. This is what makes functions stand out without a symbol table.
      size_t k = j;
      while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
      bool call = (k < n && s[k] == '(');
      if (in_words(lang_index, word, false)) {
        push(&out, Kind::kKeyword, word);
      } else if (in_words(lang_index, word, true)) {
        push(&out, Kind::kType, word);
      } else if (call) {
        push(&out, Kind::kFunction, word);
      } else {
        push(&out, Kind::kPlain, word);
      }
      i = j;
      line_start = false;
      continue;
    }

    // --- plain run: consume until something interesting ---
    {
      size_t j = i;
      while (j < n) {
        char d = s[j];
        if (is_ident_start(d) || is_digit(d)) break;
        if (d == '"' || d == '\'' || d == '`') break;
        if (d == '/' && (spec.flags & kBlockComments) && j + 1 < n && s[j + 1] == '*') break;
        bool marker = false;
        for (size_t m = 0; m < markers.size(); ++m) {
          if (!markers[m].empty() && starts_with_at(s, j, markers[m].c_str())) {
            marker = true;
            break;
          }
        }
        if (marker) break;
        if ((spec.flags & kCPreproc) && d == '#' && line_start) break;
        if ((spec.flags & kHashAttrs) && d == '#' && j + 1 < n && s[j + 1] == '[') break;
        if (d == '\n') line_start = true;
        ++j;
      }
      plain_to(i, j);
      // Track "first non-space on this line" for the preprocessor check.
      for (size_t k = i; k < j; ++k) {
        if (s[k] == '\n') line_start = true;
        else if (!is_space(s[k])) line_start = false;
      }
      if (j == i) {
        // Nothing consumed (should not happen): emit one char to make progress.
        push(&out, Kind::kPlain, std::string(1, s[i]));
        ++j;
      }
      i = j;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Markup (HTML / XML / Vue / Svelte)
// ---------------------------------------------------------------------------
std::vector<Span> tokenize_markup(const std::string& s) {
  std::vector<Span> out;
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    if (s.compare(i, 4, "<!--") == 0) {
      size_t e = s.find("-->", i + 4);
      e = (e == std::string::npos) ? n : e + 3;
      push(&out, Kind::kComment, s.substr(i, e - i));
      i = e;
      continue;
    }
    if (s[i] == '<') {
      // tag open: <name, </name, <!doctype
      size_t j = i + 1;
      bool closing = false;
      if (j < n && s[j] == '/') {
        closing = true;
        ++j;
      }
      if (j < n && (is_ident_start(s[j]))) {
        size_t k = j;
        while (k < n && (is_ident_char(s[k]) || s[k] == '-' || s[k] == ':')) ++k;
        push(&out, Kind::kPlain, s.substr(i, j - i));  // '<' or '</'
        push(&out, Kind::kTag, s.substr(j, k - j));
        i = k;
        // attributes until '>' 
        while (i < n && s[i] != '>' && s[i] != '\n') {
          if (s[i] == '/' ) {
            push(&out, Kind::kPlain, "/");
            ++i;
            continue;
          }
          if (is_space(s[i])) {
            size_t k2 = i;
            while (k2 < n && is_space(s[k2])) ++k2;
            push(&out, Kind::kPlain, s.substr(i, k2 - i));
            i = k2;
            continue;
          }
          if (is_ident_start(s[i])) {
            size_t a = i;
            while (a < n && (is_ident_char(s[a]) || s[a] == '-' || s[a] == ':' || s[a] == '.')) ++a;
            push(&out, Kind::kAttr, s.substr(i, a - i));
            i = a;
            continue;
          }
          if (s[i] == '=') {
            push(&out, Kind::kPlain, "=");
            ++i;
            // value: quoted or bare
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
              size_t e = scan_quoted(s, i, s[i], false);
              push(&out, Kind::kString, s.substr(i, e - i));
              i = e;
            } else {
              size_t a = i;
              while (a < n && !is_space(s[a]) && s[a] != '>') ++a;
              push(&out, Kind::kString, s.substr(i, a - i));
              i = a;
            }
            continue;
          }
          push(&out, Kind::kPlain, std::string(1, s[i]));
          ++i;
        }
        continue;
      }
      // doctype / processing instruction: colour to '>'
      size_t e = s.find('>', i);
      e = (e == std::string::npos) ? n : e + 1;
      push(&out, Kind::kMeta, s.substr(i, e - i));
      i = e;
      (void)closing;
      continue;
    }
    // text run up to the next '<'
    size_t e = s.find('<', i);
    if (e == std::string::npos) e = n;
    push(&out, Kind::kPlain, s.substr(i, e - i));
    i = e;
  }
  return out;
}

// ---------------------------------------------------------------------------
// CSS
// ---------------------------------------------------------------------------
std::vector<Span> tokenize_css(const std::string& s, const LangSpec& spec) {
  std::vector<Span> out;
  size_t i = 0;
  size_t n = s.size();
  int depth = 0;
  while (i < n) {
    if (s.compare(i, 2, "/*") == 0) {
      size_t e = s.find("*/", i + 2);
      e = (e == std::string::npos) ? n : e + 2;
      push(&out, Kind::kComment, s.substr(i, e - i));
      i = e;
      continue;
    }
    if (s.compare(i, 2, "//") == 0) {
      size_t e = s.find('\n', i);
      if (e == std::string::npos) e = n;
      push(&out, Kind::kComment, s.substr(i, e - i));
      i = e;
      continue;
    }
    if (s[i] == '"' || s[i] == '\'') {
      size_t e = scan_quoted(s, i, s[i], false);
      push(&out, Kind::kString, s.substr(i, e - i));
      i = e;
      continue;
    }
    if (s[i] == '@' ) {
      size_t j = i + 1;
      while (j < n && (is_ident_char(s[j]) || s[j] == '-')) ++j;
      push(&out, Kind::kPreproc, s.substr(i, j - i));
      i = j;
      continue;
    }
    if (s[i] == '{') {
      ++depth;
      push(&out, Kind::kPlain, "{");
      ++i;
      continue;
    }
    if (s[i] == '}') {
      if (depth > 0) --depth;
      push(&out, Kind::kPlain, "}");
      ++i;
      continue;
    }
    // '#' colour or id selector
    if (s[i] == '#') {
      size_t j = i + 1;
      while (j < n && (is_ident_char(s[j]) || s[j] == '-')) ++j;
      push(&out, depth == 0 ? Kind::kTag : Kind::kNumber, s.substr(i, j - i));
      i = j;
      continue;
    }
    if (s[i] == '.' && depth == 0 && i + 1 < n && is_ident_start(s[i + 1])) {
      size_t j = i + 1;
      while (j < n && (is_ident_char(s[j]) || s[j] == '-')) ++j;
      push(&out, Kind::kTag, s.substr(i, j - i));
      i = j;
      continue;
    }
    if (is_ident_start(s[i]) || s[i] == '-' || s[i] == '$') {
      size_t j = i;
      if (s[j] == '-' || s[j] == '$') ++j;
      while (j < n && (is_ident_char(s[j]) || s[j] == '-')) ++j;
      std::string word = s.substr(i, j - i);
      // inside a block and followed by ':' -> property
      size_t k = j;
      while (k < n && is_space(s[k])) ++k;
      if (depth > 0 && k < n && s[k] == ':') {
        push(&out, Kind::kAttr, word);
      } else {
        push(&out, Kind::kTag, word);
      }
      i = j;
      continue;
    }
    if (is_digit(s[i]) ||
        (s[i] == '.' && i + 1 < n && is_digit(s[i + 1]))) {
      size_t e = scan_number(s, i);
      while (e < n && (is_ident_char(s[e]) || s[e] == '%')) ++e;
      push(&out, Kind::kNumber, s.substr(i, e - i));
      i = e;
      continue;
    }
    push(&out, Kind::kPlain, std::string(1, s[i]));
    ++i;
  }
  (void)spec;
  return out;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
std::vector<Span> tokenize_json(const std::string& s, const LangSpec& spec) {
  std::vector<Span> out;
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    char c = s[i];
    if (is_space(c)) {
      size_t j = i;
      while (j < n && is_space(s[j])) ++j;
      push(&out, Kind::kPlain, s.substr(i, j - i));
      i = j;
      continue;
    }
    if ((spec.flags & kBlockComments) && s.compare(i, 2, "/*") == 0) {
      size_t e = s.find("*/", i + 2);
      e = (e == std::string::npos) ? n : e + 2;
      push(&out, Kind::kComment, s.substr(i, e - i));
      i = e;
      continue;
    }
    if (spec.line_comments && s.compare(i, 2, "//") == 0) {
      size_t e = s.find('\n', i);
      if (e == std::string::npos) e = n;
      push(&out, Kind::kComment, s.substr(i, e - i));
      i = e;
      continue;
    }
    if (c == '"') {
      size_t e = scan_quoted(s, i, '"', false);
      // a string followed by ':' is a KEY
      size_t k = e;
      while (k < n && is_space(s[k])) ++k;
      push(&out, (k < n && s[k] == ':') ? Kind::kAttr : Kind::kString,
           s.substr(i, e - i));
      i = e;
      continue;
    }
    if (is_digit(c) || (c == '-' && i + 1 < n && is_digit(s[i + 1]))) {
      size_t e = i + 1;
      while (e < n && (is_digit(s[e]) || s[e] == '.' || s[e] == 'e' ||
                       s[e] == 'E' || s[e] == '+' || s[e] == '-')) ++e;
      push(&out, Kind::kNumber, s.substr(i, e - i));
      i = e;
      continue;
    }
    if (is_ident_start(c)) {
      size_t j = i;
      while (j < n && is_ident_char(s[j])) ++j;
      std::string w = s.substr(i, j - i);
      push(&out, (w == "true" || w == "false" || w == "null") ? Kind::kKeyword
                                                              : Kind::kPlain,
           w);
      i = j;
      continue;
    }
    push(&out, Kind::kPlain, std::string(1, c));
    ++i;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------
std::vector<Span> tokenize_markdown(const std::string& s) {
  std::vector<Span> out;
  size_t i = 0;
  size_t n = s.size();
  bool at_line_start = true;
  bool in_fence = false;
  while (i < n) {
    size_t line_end = s.find('\n', i);
    size_t e = (line_end == std::string::npos) ? n : line_end;
    std::string line = s.substr(i, e - i);
    if (line.size() >= 3 && line.compare(0, 3, "```") == 0) {
      push(&out, Kind::kMeta, line);
      in_fence = !in_fence;
      if (e < n) push(&out, Kind::kPlain, "\n");
      i = (e < n) ? e + 1 : n;
      at_line_start = true;
      continue;
    }
    if (in_fence) {
      // code inside a fence is shown plain here; the code-block renderer
      // re-highlights it with its own language when it appears in chat.
      push(&out, Kind::kString, line);
      if (e < n) push(&out, Kind::kPlain, "\n");
      i = (e < n) ? e + 1 : n;
      continue;
    }
    // heading
    if (!line.empty() && line[0] == '#') {
      size_t h = 0;
      while (h < line.size() && line[h] == '#') ++h;
      if (h < line.size() && line[h] == ' ') {
        push(&out, Kind::kMeta, line.substr(0, h + 1));
        push(&out, Kind::kTag, line.substr(h + 1));
        if (e < n) push(&out, Kind::kPlain, "\n");
        i = (e < n) ? e + 1 : n;
        at_line_start = true;
        continue;
      }
    }
    // list marker / quote / hr
    if (!line.empty() && (line[0] == '-' || line[0] == '*' || line[0] == '+' ||
                          line[0] == '>')) {
      if (line.size() > 1 && (line[1] == ' ' || line.size() > 2)) {
        push(&out, Kind::kMeta, line.substr(0, 1));
        push(&out, Kind::kPlain, line.substr(1));
        if (e < n) push(&out, Kind::kPlain, "\n");
        i = (e < n) ? e + 1 : n;
        at_line_start = true;
        continue;
      }
    }
    // inline: **bold**, `code`, [text](url)
    size_t j = 0;
    while (j < line.size()) {
      if (line[j] == '`') {
        size_t close = line.find('`', j + 1);
        if (close == std::string::npos) {
          push(&out, Kind::kPlain, line.substr(j));
          break;
        }
        push(&out, Kind::kPlain, "`");
        push(&out, Kind::kString, line.substr(j + 1, close - j - 1));
        push(&out, Kind::kPlain, "`");
        j = close + 1;
        continue;
      }
      if (j + 1 < line.size() && line[j] == '*' && line[j + 1] == '*') {
        size_t close = line.find("**", j + 2);
        if (close == std::string::npos) {
          push(&out, Kind::kPlain, line.substr(j));
          break;
        }
        push(&out, Kind::kMeta, "**");
        push(&out, Kind::kType, line.substr(j + 2, close - j - 2));
        push(&out, Kind::kMeta, "**");
        j = close + 2;
        continue;
      }
      push(&out, Kind::kPlain, std::string(1, line[j]));
      ++j;
    }
    if (e < n) push(&out, Kind::kPlain, "\n");
    i = (e < n) ? e + 1 : n;
    at_line_start = true;
  }
  (void)at_line_start;
  return out;
}

// ---------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------
std::vector<Span> tokenize_diff(const std::string& s) {
  std::vector<Span> out;
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    size_t line_end = s.find('\n', i);
    size_t e = (line_end == std::string::npos) ? n : line_end;
    std::string line = s.substr(i, e - i);
    Kind k = Kind::kPlain;
    if (line.size() >= 2 && line.compare(0, 2, "++") == 0) k = Kind::kMeta;
    else if (line.size() >= 2 && line.compare(0, 2, "--") == 0) k = Kind::kMeta;
    else if (line.size() >= 2 && line.compare(0, 2, "@@") == 0) k = Kind::kPreproc;
    else if (!line.empty() && line[0] == '+') k = Kind::kAdded;
    else if (!line.empty() && line[0] == '-') k = Kind::kRemoved;
    else if (!line.empty() && (line.compare(0, 4, "diff") == 0 ||
                               line.compare(0, 5, "index") == 0)) k = Kind::kMeta;
    push(&out, k, line);
    if (e < n) push(&out, Kind::kPlain, "\n");
    i = (e < n) ? e + 1 : n;
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
const LangSpec* find_lang(const std::string& id) {
  if (id.empty()) return &kPlainSpec;
  std::string want = lower_ascii(id);
  for (size_t i = 0; i < kLangCount; ++i) {
    if (want == kLangs[i].id) return &kLangs[i];
  }
  for (size_t i = 0; i < sizeof(kAliases) / sizeof(kAliases[0]); ++i) {
    if (want == kAliases[i].from) {
      std::string to = kAliases[i].to;
      if (to == "plain") return &kPlainSpec;
      for (size_t k = 0; k < kLangCount; ++k) {
        if (to == kLangs[k].id) return &kLangs[k];
      }
    }
  }
  return &kPlainSpec;
}

std::vector<std::string> all_langs() {
  std::vector<std::string> out;
  for (size_t i = 0; i < kLangCount; ++i) out.push_back(kLangs[i].id);
  return out;
}

std::string lang_for_path(const std::string& path) {
  std::string name = path;
  size_t cut = name.find_last_of("/\\");
  if (cut != std::string::npos) name = name.substr(cut + 1);
  std::string lower = lower_ascii(name);
  // exact names first (no extension to split)
  if (lower == "dockerfile") return "docker";
  if (lower == "makefile" || lower == "gnumakefile") return "makefile";
  if (lower == "cmakelists.txt") return "cmake";
  size_t dot = lower.rfind('.');
  if (dot == std::string::npos) return std::string();
  std::string ext = lower.substr(dot + 1);
  const LangSpec* spec = find_lang(ext);
  return spec && spec != &kPlainSpec ? std::string(spec->id) : std::string();
}

std::vector<Span> tokenize(const std::string& code, const std::string& lang) {
  const LangSpec* spec = find_lang(lang);
  if (!spec || spec == &kPlainSpec) {
    std::vector<Span> out;
    push(&out, Kind::kPlain, code);
    return out;
  }
  switch (spec->syntax) {
    case Syntax::kMarkup:
      return tokenize_markup(code);
    case Syntax::kCss:
      return tokenize_css(code, *spec);
    case Syntax::kJson:
      return tokenize_json(code, *spec);
    case Syntax::kMarkdown:
      return tokenize_markdown(code);
    case Syntax::kDiff:
      return tokenize_diff(code);
    case Syntax::kGeneric:
    default:
      return tokenize_generic(code, *spec, index_of(spec));
  }
}

std::string escape_html(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  for (size_t i = 0; i < s.size(); ++i) {
    switch (s[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(s[i]); break;
    }
  }
  return out;
}

namespace {
const char* class_for(Kind k) {
  switch (k) {
    case Kind::kKeyword: return "hl-kw";
    case Kind::kType: return "hl-ty";
    case Kind::kString: return "hl-st";
    case Kind::kNumber: return "hl-nu";
    case Kind::kComment: return "hl-cm";
    case Kind::kPreproc: return "hl-pp";
    case Kind::kFunction: return "hl-fn";
    case Kind::kAttr: return "hl-at";
    case Kind::kTag: return "hl-tg";
    case Kind::kMeta: return "hl-me";
    case Kind::kAdded: return "hl-ad";
    case Kind::kRemoved: return "hl-de";
    case Kind::kPlain:
    default: return nullptr;
  }
}
}  // namespace

std::string to_html(const std::string& code, const std::string& lang) {
  std::vector<Span> spans = tokenize(code, lang);
  std::string out;
  out.reserve(code.size() + code.size() / 3);
  for (size_t i = 0; i < spans.size(); ++i) {
    const char* cls = class_for(spans[i].kind);
    std::string esc = escape_html(spans[i].text);
    if (!cls) {
      out += esc;
    } else {
      out += "<span class=\"";
      out += cls;
      out += "\">";
      out += esc;
      out += "</span>";
    }
  }
  return out;
}

const std::string& to_html_cached(const std::string& code, const std::string& lang) {
  // (lang, content) -> html, with a small FIFO eviction. The UI re-renders on
  // every timer tick while a turn streams, so the SAME block is asked for many
  // times per second; tokenizing it again each time would be pure waste.
  struct Key {
    std::string lang;
    uint64_t hash;
    bool operator<(const Key& o) const {
      if (hash != o.hash) return hash < o.hash;
      return lang < o.lang;
    }
  };
  static std::map<Key, std::string> cache;
  static std::deque<Key> order;
  static std::mutex mu;
  const size_t kMax = 96;

  uint64_t h = 1469598103934665603ULL;  // FNV-1a 64
  for (size_t i = 0; i < code.size(); ++i) {
    h ^= static_cast<unsigned char>(code[i]);
    h *= 1099511628211ULL;
  }
  Key key;
  key.lang = lang;
  key.hash = h;

  std::lock_guard<std::mutex> lock(mu);
  std::map<Key, std::string>::iterator it = cache.find(key);
  if (it != cache.end()) return it->second;
  std::string html = to_html(code, lang);
  if (cache.size() >= kMax) {
    cache.erase(order.front());
    order.pop_front();
  }
  std::pair<std::map<Key, std::string>::iterator, bool> ins =
      cache.insert(std::make_pair(key, html));
  order.push_back(key);
  return ins.first->second;
}

}  // namespace highlight
}  // namespace fs
