// test_highlight.cpp - syntax highlighter: token classes, escaping, cache.
//
// The invariant that matters most is the ROUND TRIP: concatenating every span's
// text must reproduce the input byte for byte. Highlighting is a display layer,
// and the one thing it must never do is change what the code says.
#include "common/fs_highlight.hpp"
#include "test_framework.hpp"

#include <string>
#include <vector>

using namespace fs::highlight;

namespace {

std::string joined(const std::string& code, const std::string& lang) {
  std::vector<Span> sp = tokenize(code, lang);
  std::string out;
  for (size_t i = 0; i < sp.size(); ++i) out += sp[i].text;
  return out;
}

bool has_kind(const std::string& code, const std::string& lang, Kind kind,
              const std::string& needle) {
  std::vector<Span> sp = tokenize(code, lang);
  for (size_t i = 0; i < sp.size(); ++i) {
    if (sp[i].kind == kind && sp[i].text == needle) return true;
  }
  return false;
}

bool html_has(const std::string& code, const std::string& lang, const char* cls) {
  return to_html(code, lang).find(cls) != std::string::npos;
}

}  // namespace

FS_TEST(hl_escape_html) {
  EXPECT_EQ(escape_html("a<b>&\"c\"'"), std::string("a&lt;b&gt;&amp;&quot;c&quot;&#39;"));
  EXPECT_EQ(escape_html("plain"), std::string("plain"));
}

FS_TEST(hl_round_trip_never_loses_text) {
  // Includes the shapes that used to break a naive scanner: an unterminated
  // string, a lone apostrophe, a backtick that is not a string, CRLF, and code
  // that is not valid in ANY language.
  const char* samples[] = {
      "int main() { return 0; }\n",
      "#include <vector>\n// c\n",
      "def f(x):\n    \"\"\"doc\"\"\"\n    return x\n",
      "let s = r#\"raw \"quoted\" text\"#;\n",
      "key = \"unterminated\nnext();\n",
      "it's a lone apostrophe in text\n",
      "SELECT * FROM t -- trailing\n",
      "<div class=\"a\">text & more</div>\n",
      ".a{color:#fff;margin:0 4px}\n",
      "{\"a\":[1,true,null]}\n",
      "# h\n- x\n```py\nx=1\n```\n",
      "@@ -1 +1 @@\n-old\n+new\n",
      "\xE4\xB8\xAD\xE6\x96\x87 comment\n",
      "a\r\nb\r\n",
      "",
  };
  const char* langs[] = {"cpp", "python", "rust", "javascript", "html", "css",
                         "json", "markdown", "diff", "sql_ci", "yaml", "bash",
                         "ruby", "plain", "no-such-lang"};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    for (size_t l = 0; l < sizeof(langs) / sizeof(langs[0]); ++l) {
      EXPECT_EQ(joined(samples[i], langs[l]), std::string(samples[i]));
    }
  }
}

FS_TEST(hl_cpp_classes) {
  const std::string code =
      "#include <string>\n"
      "// note\n"
      "int main(int argc) {\n"
      "  std::string s = \"hi\";\n"
      "  return 42;\n"
      "}\n";
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kPreproc, "#include <string>"));
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kComment, "// note"));
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kType, "int"));
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kFunction, "main"));
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kString, "\"hi\""));
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kNumber, "42"));
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kKeyword, "return"));
}

FS_TEST(hl_cpp_raw_string_and_char) {
  // R"(..)" must swallow embedded quotes; 'a' must not swallow the next line.
  const std::string code = "auto r = R\"(a \"b\" c)\"; char q = 'x';\nint y;\n";
  EXPECT_EQ(joined(code, "cpp"), code);
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kString, "R\"(a \"b\" c)\""));
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kString, "'x'"));
  // The declaration after the char literal still tokenizes normally.
  EXPECT_TRUE(has_kind(code, "cpp", Kind::kType, "int"));
}

FS_TEST(hl_python_comment_vs_triple_string) {
  const std::string code =
      "def f():\n"
      "    \"\"\"doc # not a comment\"\"\"\n"
      "    return 1  # real comment\n";
  EXPECT_TRUE(has_kind(code, "python", Kind::kString, "\"\"\"doc # not a comment\"\"\""));
  EXPECT_TRUE(has_kind(code, "python", Kind::kComment, "# real comment"));
  EXPECT_TRUE(has_kind(code, "python", Kind::kKeyword, "def"));
  EXPECT_TRUE(has_kind(code, "python", Kind::kFunction, "f"));
}

FS_TEST(hl_json_key_vs_value) {
  const std::string code = "{\"name\": \"value\", \"n\": 3, \"ok\": true}";
  EXPECT_TRUE(has_kind(code, "json", Kind::kAttr, "\"name\""));
  EXPECT_TRUE(has_kind(code, "json", Kind::kString, "\"value\""));
  EXPECT_TRUE(has_kind(code, "json", Kind::kNumber, "3"));
  EXPECT_TRUE(has_kind(code, "json", Kind::kKeyword, "true"));
}

FS_TEST(hl_markup_tags_and_attrs) {
  const std::string code = "<a href=\"x\" data-id='1'>t</a><!-- c -->";
  EXPECT_TRUE(has_kind(code, "html", Kind::kTag, "a"));
  EXPECT_TRUE(has_kind(code, "html", Kind::kAttr, "href"));
  EXPECT_TRUE(has_kind(code, "html", Kind::kString, "\"x\""));
  EXPECT_TRUE(has_kind(code, "html", Kind::kComment, "<!-- c -->"));
  // Markup text is never swallowed into a tag.
  EXPECT_EQ(joined(code, "html"), code);
}

FS_TEST(hl_css_property_vs_selector) {
  const std::string code = ".btn { color: #fff; margin: 0 4px; }";
  EXPECT_TRUE(has_kind(code, "css", Kind::kTag, ".btn"));
  EXPECT_TRUE(has_kind(code, "css", Kind::kAttr, "color"));
  EXPECT_TRUE(has_kind(code, "css", Kind::kNumber, "#fff"));
  EXPECT_TRUE(has_kind(code, "css", Kind::kAttr, "margin"));
}

FS_TEST(hl_diff_added_removed) {
  const std::string code =
      "diff --git a/x b/x\n"
      "@@ -1 +1 @@\n"
      "-old\n"
      "+new\n";
  EXPECT_TRUE(has_kind(code, "diff", Kind::kMeta, "diff --git a/x b/x"));
  EXPECT_TRUE(has_kind(code, "diff", Kind::kPreproc, "@@ -1 +1 @@"));
  EXPECT_TRUE(has_kind(code, "diff", Kind::kRemoved, "-old"));
  EXPECT_TRUE(has_kind(code, "diff", Kind::kAdded, "+new"));
}

FS_TEST(hl_markdown_markers) {
  const std::string code = "# Title\n\n- item\n\n`code` and **bold**\n";
  EXPECT_TRUE(has_kind(code, "markdown", Kind::kMeta, "# "));
  EXPECT_TRUE(has_kind(code, "markdown", Kind::kMeta, "-"));
  EXPECT_TRUE(has_kind(code, "markdown", Kind::kString, "code"));
  EXPECT_TRUE(has_kind(code, "markdown", Kind::kType, "bold"));
}

FS_TEST(hl_unknown_language_is_plain) {
  const std::string code = "int x = 1; // not highlighted";
  std::vector<Span> sp = tokenize(code, "definitely-not-a-language");
  ASSERT_TRUE(sp.size() == 1);
  EXPECT_TRUE(sp[0].kind == Kind::kPlain);
  EXPECT_EQ(sp[0].text, code);
  // Plain HTML is escaped, never emitted as markup.
  EXPECT_EQ(to_html("<b>", "plain"), std::string("&lt;b&gt;"));
}

FS_TEST(hl_language_aliases_and_paths) {
  EXPECT_TRUE(find_lang("C++") != nullptr);
  EXPECT_EQ(std::string(find_lang("c++")->id), std::string("cpp"));
  EXPECT_EQ(std::string(find_lang("py")->id), std::string("python"));
  EXPECT_EQ(std::string(find_lang("TS")->id), std::string("typescript"));
  EXPECT_EQ(std::string(find_lang("sh")->id), std::string("bash"));
  EXPECT_EQ(std::string(find_lang("yml")->id), std::string("yaml"));
  EXPECT_EQ(std::string(find_lang("nonsense")->id), std::string("plain"));
  EXPECT_EQ(lang_for_path("src/main.cpp"), std::string("cpp"));
  EXPECT_EQ(lang_for_path("C:\\a\\b\\App.tsx"), std::string("tsx"));
  EXPECT_EQ(lang_for_path("Makefile"), std::string("makefile"));
  EXPECT_EQ(lang_for_path("Dockerfile"), std::string("docker"));
  EXPECT_EQ(lang_for_path("CMakeLists.txt"), std::string("cmake"));
  EXPECT_EQ(lang_for_path("notes.unknownext"), std::string(""));
  EXPECT_EQ(lang_for_path("noext"), std::string(""));
}

FS_TEST(hl_html_output_and_cache) {
  std::string h = to_html("int x;", "cpp");
  EXPECT_TRUE(h.find("<span class=\"hl-ty\">int</span>") != std::string::npos);
  // Escaping happens inside the spans: a string containing markup cannot
  // inject an element into the page.
  std::string evil = to_html("const char* s = \"<script>\";", "cpp");
  EXPECT_TRUE(evil.find("<script>") == std::string::npos);
  EXPECT_TRUE(evil.find("&lt;script&gt;") != std::string::npos);

  // The cache must return the SAME object for the same (code, lang): the UI
  // asks for every visible block on every render.
  const std::string& a = to_html_cached("int y;", "cpp");
  const std::string& b = to_html_cached("int y;", "cpp");
  EXPECT_TRUE(&a == &b);
  // Different language, same text -> different entry.
  const std::string& c = to_html_cached("int y;", "python");
  EXPECT_TRUE(&a != &c);
}

FS_TEST(hl_all_languages_tokenize) {
  std::vector<std::string> all = all_langs();
  EXPECT_TRUE(all.size() >= 50);
  const std::string code = "x = 1 // c\n# h\n{\"a\": 1}\n<a b='c'>d</a>\n";
  for (size_t i = 0; i < all.size(); ++i) {
    EXPECT_EQ(joined(code, all[i]), code);
    // No language may produce an unescaped '<' in its HTML.
    std::string h = to_html(code, all[i]);
    EXPECT_TRUE(h.find("<a ") == std::string::npos);
  }
}
