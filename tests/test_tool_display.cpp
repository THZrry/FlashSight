// test_tool_display.cpp - human-facing tool labels are generated from the
// result data, not serialized into the model-facing JSON.
#include "test_framework.hpp"

#include "common/fs_json.hpp"
#include "ui/ui_tool_display.hpp"

using namespace fs;
using namespace fs::ui;

FS_TEST(tool_display_read_and_edit) {
  Json args = Json::object();
  args["path"] = "src/main.cpp";
  std::string detail = Json{{"startLine", 3}, {"endLine", 40},
                            {"totalLines", 200}, {"content", "..."}}
                          .dump();
  EXPECT_EQ(tool_display_summary("read_file", args, detail, true),
            std::string("src/main.cpp 3-40/200 lines"));

  Json eargs = Json{{"edits", Json::array({Json::object(), Json::object()})}};
  std::string edetail =
      Json{{"changes", Json::array({Json::object(), Json::object()})},
           {"warnings", Json::array({"read-back differs"})}}
          .dump();
  EXPECT_EQ(tool_display_summary("edit_file", eargs, edetail, true),
            std::string("2 edit(s); 1 warning(s)"));
}

FS_TEST(tool_display_paths_commands_and_agents) {
  Json largs = Json::object();
  largs["path"] = "src/ui";
  EXPECT_EQ(tool_display_summary("list_dir", largs, "{}", true),
            std::string("src/ui"));

  Json targs = Json::object();
  targs["command"] = "gcc -c src/main.cpp";
  EXPECT_EQ(tool_display_summary("terminal_exec", targs, "{}", true),
            std::string("gcc -c src/main.cpp"));

  std::string sdetail = Json{{"id", "sa_7"}, {"status", "failed"}}.dump();
  EXPECT_EQ(tool_display_summary("subagent_spawn", Json::object(), sdetail, true),
            std::string("sa_7 · failed"));
}

FS_TEST(tool_display_todo_and_plan) {
  EXPECT_EQ(tool_display_summary(
                "todo_add", Json::object(),
                Json{{"added", 2}, {"pending", 3}}.dump(), true),
            std::string("+2 · 3 pending"));
  EXPECT_EQ(tool_display_summary(
                "todo_check", Json::object(),
                Json{{"id", 4}, {"done", true}}.dump(), true),
            std::string("4 done"));
  EXPECT_EQ(tool_display_summary(
                "todo_erase", Json::object(),
                Json{{"removed", 4}}.dump(), true),
            std::string("#4 removed"));
  EXPECT_EQ(tool_display_summary("todo_clear", Json::object(), "{}", true),
            std::string("cleared"));
  EXPECT_EQ(tool_display_summary(
                "plan_update", Json::object(),
                Json{{"seq", 3}, {"exists", true}, {"status", "done"}}.dump(),
                true),
            std::string("#3 · done"));
  EXPECT_EQ(tool_display_summary("read_file", Json::object(), "{}", false),
            std::string("failed"));
  EXPECT_EQ(tool_display_summary("fetch_url", Json::object(), "{}", false),
            std::string("failed"));
}
