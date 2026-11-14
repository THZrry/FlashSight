// test_addon2.cpp - addon2 后端增强单测（工具补全 / 健壮性 / 长任务 / skill）
// 覆盖：ls 基础/详细、check_dir、regex 替换、噪声检测、写权限门、
//       进程 todo（含子代理独立树）、中断-继续、计划评审 revise、工作流图、双语 skill
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <ghc/filesystem.hpp>

#include "test_framework.hpp"

#include "agent/agent_loop.hpp"
#include "agent/todo_store.hpp"
#include "common/fs_clock.hpp"
#include "common/fs_path.hpp"
#include "core/fs_api.hpp"
#include "core/workflow.hpp"
#include "tools/check_dir_tools.hpp"
#include "tools/file_tools.hpp"
#include "tools/noise_detect.hpp"
#include "tools/regex_tools.hpp"
#include "tools/skill_registry.hpp"
#include "tools/todo_tools.hpp"

using namespace fs;

namespace {

class FakeClockA2 : public Clock {
 public:
  explicit FakeClockA2(ClockMs start = 5000) : now_(start) {}
  ClockMs now_ms() const override { return now_; }
  void advance(ClockMs d) { now_ += d; }

 private:
  ClockMs now_;
};

void write_file_bin(const ghc::filesystem::path& p, const std::string& s) {
  std::ofstream ofs(p.string(), std::ios::binary);
  ofs << s;
}

std::string read_file_bin(const ghc::filesystem::path& p) {
  std::ifstream ifs(p.string(), std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  return s;
}

}  // namespace

// ---------- list_dir：只有一份 JSON entries ----------
FS_TEST(addon2_ls_basic_and_detail) {
  fstest::TempDir td("a2ls");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  write_file_bin(proj / "alpha.txt", "a");
  write_file_bin(proj / "beta.txt", "bb");
  ghc::filesystem::create_directories(proj / "sub");

  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();

  FsResult base = reg.invoke(ctx, "list_dir", Json::object());
  ASSERT_TRUE(base.ok());
  ASSERT_TRUE(base.data.contains("entries") && base.data["entries"].is_array());
  // The same listing must not be repeated as a string in every result.
  EXPECT_FALSE(base.data.contains("names"));
  EXPECT_FALSE(base.data.contains("entriesCsv"));
  bool alpha = false, sub = false;
  for (auto& e : base.data["entries"]) {
    if (e.value("name", "") == "alpha.txt") alpha = true;
    if (e.value("name", "") == "sub" && e.value("isDir", false)) sub = true;
    EXPECT_TRUE(e.contains("size"));
    EXPECT_TRUE(e.contains("mtime"));
  }
  EXPECT_TRUE(alpha);
  EXPECT_TRUE(sub);
  EXPECT_EQ(base.data["path"].get<std::string>(), std::string("."));

  Json dargs = Json::object();
  dargs["detail"] = true;
  FsResult det = reg.invoke(ctx, "list_dir", dargs);
  ASSERT_TRUE(det.ok());
  EXPECT_TRUE(det.data["detail"].get<bool>());
  EXPECT_FALSE(det.data.contains("entriesCsv"));
  EXPECT_TRUE(det.data["entries"].is_array());
}

// ---------- check_dir：子代理筛目录（复用 ls，带 requirement）----------
FS_TEST(addon2_check_dir) {
  fstest::TempDir td("a2cd");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  write_file_bin(proj / "x.hpp", "h");
  write_file_bin(proj / "y.cpp", "c");

  ToolRegistry reg;
  register_file_tools(&reg);
  register_check_dir_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();

  // requirement 必填
  EXPECT_FALSE(reg.invoke(ctx, "check_dir", Json::object()).ok());

  Json args = Json::object();
  args["requirement"] = "找所有 .hpp 头文件";
  FsResult r = reg.invoke(ctx, "check_dir", args);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data["requirement"].get<std::string>(), std::string("找所有 .hpp 头文件"));
  EXPECT_TRUE(r.data.contains("entries"));
  EXPECT_TRUE(r.data.contains("hint"));
}

// ---------- file_regex：多文件替换 + $1 分组 + 无匹配不动 ----------
FS_TEST(addon2_regex_replace) {
  fstest::TempDir td("a2rx");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);
  write_file_bin(proj / "one.txt", "foo=1\nbar=2\n");
  write_file_bin(proj / "two.txt", "no match here\n");

  ToolRegistry reg;
  register_regex_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();

  Json args = Json::object();
  args["files"] = Json::array({"one.txt", "two.txt"});
  args["pattern"] = "(\\w+)=(\\d+)";
  args["replacement"] = "$1 -> $2";
  args["flags"] = "g";
  FsResult r = reg.invoke(ctx, "file_regex", args);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ((int)r.data["totalReplacements"].get<int64_t>(), 2);
  std::string after = read_file_bin(proj / "one.txt");
  EXPECT_EQ(after, std::string("foo -> 1\nbar -> 2\n"));
  // two.txt 无匹配：内容不变
  EXPECT_EQ(read_file_bin(proj / "two.txt"), std::string("no match here\n"));

  // 逃逸路径拒绝
  Json bad = Json::object();
  bad["files"] = Json::array({"../escape.txt"});
  bad["pattern"] = "a";
  bad["replacement"] = "b";
  FsResult rb = reg.invoke(ctx, "file_regex", bad);
  ASSERT_TRUE(rb.ok());  // 结果里带 per-file 错误
  EXPECT_FALSE(rb.data["results"][0]["ok"].get<bool>());
}

// ---------- 噪声检测：异区短串命中；正常中文/英文不误报 ----------
FS_TEST(addon2_noise_detect) {
  // 正常英文 + 中文注释：不报
  NoiseReport ok1 = detect_noise("// normal English comment\nint x = 1;\n");
  EXPECT_FALSE(ok1.noisy);
  NoiseReport ok2 = detect_noise("// 正常中文注释\nint y = 2;\n");
  EXPECT_FALSE(ok2.noisy);

  // 正常内容后突然出现阿拉伯字符短串：命中
  NoiseReport bad = detect_noise("std::string s = \"hello\";  \u0627\u0644  more;\n");
  EXPECT_TRUE(bad.noisy);
  EXPECT_TRUE(bad.suspicious_chars >= 2);
}

// ---------- 写权限门：非 full 权限拒绝绝对路径；full 放行 ----------
FS_TEST(addon2_write_permission_gate) {
  fstest::TempDir td("a2perm");
  ghc::filesystem::path proj = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj);

  ToolRegistry reg;
  register_file_tools(&reg);
  ToolContext ctx;
  ctx.cwd = proj.string();
  ctx.permission = "permissive";

  // 绝对路径（项目外）在 permissive 下拒绝
  ghc::filesystem::path outside = ghc::filesystem::path(td.path()) / "outside.txt";
  Json wargs = Json::object();
  wargs["path"] = outside.string();
  wargs["content"] = "x";
  FsResult r = reg.invoke(ctx, "write_file", wargs);
  EXPECT_FALSE(r.ok());

  // full 权限：允许项目外（逃逸检查仍在，绝对路径可用）
  ctx.permission = "full";
  FsResult r2 = reg.invoke(ctx, "write_file", wargs);
  EXPECT_TRUE(r2.ok());
  EXPECT_EQ(read_file_bin(outside), std::string("x"));
}

// ---------- 进程 todo：add/list/check + 子代理独立进程树 ----------
FS_TEST(addon2_todo_tools_and_subagent_tree) {
  AgentHostDeps deps;
  FakeClockA2 clock;
  deps.clock = &clock;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);

  // 主会话进程树
  FsResult a1 = host.todo_op("s_1", Json{{"op", "add"}, {"task", "task A"}});
  ASSERT_TRUE(a1.ok());
  int id1 = (int)a1.data["id"].get<int64_t>();
  FsResult a2 = host.todo_op("s_1", Json{{"op", "add"}, {"task", "task B"}});
  ASSERT_TRUE(a2.ok());

  FsResult l1 = host.todo_op("s_1", Json{{"op", "list"}});
  ASSERT_TRUE(l1.ok());
  EXPECT_EQ((int)l1.data["items"].size(), 2);
  EXPECT_EQ(l1.data["pending"].get<int>(), 2);

  // 勾选一项
  FsResult c = host.todo_op("s_1", Json{{"op", "check"}, {"id", id1}, {"done", true}});
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.data["pending"].get<int>(), 1);

  // 子代理独立进程树（addon2：工作流子代理也提供进程工具）
  FsResult s1 = host.todo_op("sub_7", Json{{"op", "add"}, {"task", "sub work"}});
  ASSERT_TRUE(s1.ok());
  EXPECT_EQ(host.todo_pending("s_1"), 1);
  EXPECT_EQ(host.todo_pending("sub_7"), 1);
  // 子代理勾选不影响主会话
  host.todo_op("sub_7", Json{{"op", "check"},
                             {"id", (int)s1.data["id"].get<int64_t>()}, {"done", true}});
  EXPECT_EQ(host.todo_pending("sub_7"), 0);
  EXPECT_EQ(host.todo_pending("s_1"), 1);
}

// ---------- 中断-继续：失败保存断点，续跑后成功收尾 ----------
FS_TEST(addon2_continue_task) {
  AgentLoop::Options opts;
  opts.sid = "s_1";
  opts.event = [](const std::string&, const Json&) {};
  AgentLoop loop(std::move(opts));
  int calls = 0;
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out, const std::function<void(const std::string&)>&) -> FsResult {
    ++calls;
    if (calls == 1) {
      out->error = "network hiccup";
      out->text = "";
      out->tool_calls = Json::array();
      return FsResult::error(ErrCode::kNetwork, "network hiccup");
    }
    out->text = "resumed and finished";
    out->error = "";
    out->tool_calls = Json::array();
    return FsResult::success();
  };
  // 第一次失败：保存断点
  loop.send("do the work");
  EXPECT_TRUE(loop.has_saved_context());
  // 打断后续接：发引导续跑
  FsResult r = loop.continue_task("please retry, the API was flaky");
  EXPECT_TRUE(r.ok());
  EXPECT_TRUE(loop.state() == LoopState::kTurnDone);
  EXPECT_EQ(calls, 2);
}

// ---------- 计划评审：revise 保留计划 + 评价；reject 清计划 ----------
FS_TEST(addon2_plan_review) {
  fstest::TempDir td("a2plan");
  FakeClockA2 clock(1000);
  FsApi api(ghc::filesystem::path(td.path()), &clock, nullptr);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  // 写入规划文档（不是步骤树）：正文 + 状态
  Json plan = Json::object();
  plan["content"] = "# build feature X\n\n1. a\n2. b\n";
  plan["status"] = "active";
  FsResult pu = api.plan_update(sid, plan);
  ASSERT_TRUE(pu.ok());
  EXPECT_TRUE(pu.data["exists"].get<bool>());
  EXPECT_EQ(pu.data["status"].get<std::string>(), std::string("active"));
  std::string path = pu.data["path"].get<std::string>();
  // It is a real document inside the project, not a field of full.json.
  EXPECT_TRUE(path.find(".flashsight") != std::string::npos);
  EXPECT_TRUE(path.find("plans") != std::string::npos);
  EXPECT_TRUE(ghc::filesystem::exists(ghc::filesystem::path(path)));

  // Status-only update keeps the body: that is how the model marks the plan
  // complete without re-sending the whole document.
  Json done = Json::object();
  done["status"] = "done";
  FsResult pd = api.plan_update(sid, done);
  ASSERT_TRUE(pd.ok());
  EXPECT_EQ(pd.data["status"].get<std::string>(), std::string("done"));
  EXPECT_TRUE(pd.data["content"].get<std::string>().find("build feature X") !=
              std::string::npos);
  // ...and an active status can be restored the same way.
  Json again = Json::object();
  again["status"] = "active";
  EXPECT_TRUE(api.plan_update(sid, again).ok());

  // A second task plan is a NEW numbered document; the old one stays on disk.
  Json next_plan = Json::object();
  next_plan["content"] = "# second task\n\n- c\n";
  next_plan["status"] = "active";
  next_plan["newPlan"] = true;
  FsResult np = api.plan_update(sid, next_plan);
  ASSERT_TRUE(np.ok());
  EXPECT_EQ(jsonutil::get_int(np.data, "seq", 0), int64_t(2));
  EXPECT_TRUE(np.data.contains("plans") && np.data["plans"].size() >= 2);
  EXPECT_TRUE(ghc::filesystem::exists(
      ghc::filesystem::path(np.data["path"].get<std::string>())));

  // revise：保留计划并附 review 评价
  FsResult rv = api.plan_review(sid, "revise", "add a test step please");
  ASSERT_TRUE(rv.ok());
  EXPECT_EQ(rv.data["verdict"].get<std::string>(), std::string("revise"));
  EXPECT_EQ(rv.data["plan"]["review"]["comment"].get<std::string>(),
            std::string("add a test step please"));
  // 评价作为引导注入（revise 时）
  FsResult sl = api.steering_list(sid);
  ASSERT_TRUE(sl.ok());
  EXPECT_EQ((int)sl.data.size(), 1);

  // 非法 verdict 拒绝
  EXPECT_FALSE(api.plan_review(sid, "maybe", "").ok());

  // reject：清掉规划文档
  EXPECT_TRUE(api.plan_review(sid, "reject", "not now").ok());
  FsResult pg = api.plan_get(sid);
  ASSERT_TRUE(pg.ok());
  EXPECT_FALSE(pg.data["exists"].get<bool>());
  EXPECT_FALSE(ghc::filesystem::exists(ghc::filesystem::path(pg.data["path"].get<std::string>())));
}

// ---------- 已读标记：失败红点看过之后重启也不回来 ----------
FS_TEST(addon2_session_seen_survives_restart) {
  fstest::TempDir td("a2seen");
  FakeClockA2 clock(1000);
  FsApi api(ghc::filesystem::path(td.path()), &clock, nullptr);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  int64_t updated_before = 0;
  {
    FsResult r = api.session_list(pid);
    ASSERT_TRUE(r.ok());
    for (auto& row : r.data) {
      if (row["sid"].get<std::string>() == sid) {
        updated_before = jsonutil::get_int(row, "updatedAt", 0);
      }
    }
  }
  EXPECT_TRUE(api.session_mark_seen(sid, 424242).ok());
  FsResult r2 = api.session_list(pid);
  ASSERT_TRUE(r2.ok());
  bool found = false;
  for (auto& row : r2.data) {
    if (row["sid"].get<std::string>() != sid) continue;
    found = true;
    EXPECT_EQ(jsonutil::get_int(row, "seenAt", 0), int64_t(424242));
    // Writing the marker must NOT touch full.json's mtime: the dot compares that
    // mtime with seenAt, so bumping it would make the dot reappear immediately.
    EXPECT_EQ(jsonutil::get_int(row, "updatedAt", 0), updated_before);
  }
  EXPECT_TRUE(found);
}

// ---------- 供应商是模型身份的一部分：同名模型可以分属两家 ----------
FS_TEST(addon2_session_model_provider_is_persisted) {
  fstest::TempDir td("a2prov");
  FakeClockA2 clock(1000);
  FsApi api(ghc::filesystem::path(td.path()), &clock, nullptr);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  EXPECT_TRUE(api.session_set_model(sid, "deepseek-v4.1-flash").ok());
  EXPECT_TRUE(api.session_set_provider(sid, "pr_second").ok());
  FsResult info = api.session_info(sid);
  ASSERT_TRUE(info.ok());
  ASSERT_TRUE(info.data.contains("sessionSettings"));
  const Json& ss = info.data["sessionSettings"];
  EXPECT_EQ(jsonutil::get<std::string>(ss, "model", ""),
            std::string("deepseek-v4.1-flash"));
  // The request must be able to name the SECOND provider that serves the same
  // model id, not fall back to the first one by name.
  EXPECT_EQ(jsonutil::get<std::string>(ss, "provider", ""),
            std::string("pr_second"));

  // ONE call writes the pair (model + provider) so a live auto round can never
  // observe a half-applied route.
  EXPECT_TRUE(api.session_set_model_provider(sid, "deepseek-v4.1-flash",
                                             "pr_third").ok());
  FsResult info2 = api.session_info(sid);
  ASSERT_TRUE(info2.ok());
  const Json& ss2 = info2.data["sessionSettings"];
  EXPECT_EQ(jsonutil::get<std::string>(ss2, "model", ""),
            std::string("deepseek-v4.1-flash"));
  EXPECT_EQ(jsonutil::get<std::string>(ss2, "provider", ""),
            std::string("pr_third"));
}

// ---------- 工作流：定义/启动/图结构/步骤重试跳过 ----------
FS_TEST(addon2_workflow_backend) {
  FakeClockA2 clock(2000);
  WorkflowStore ws(&clock);
  for (auto& d : builtin_workflows()) ws.define(d.to_json());

  Json defs = ws.list_defs();
  ASSERT_TRUE(defs.is_array());
  EXPECT_TRUE(defs.size() >= 1);
  const WorkflowDef* def = ws.find_def("research-implement");
  ASSERT_TRUE(def != nullptr);

  WorkflowRun* run = ws.create_run(*def, "s_1");
  ASSERT_TRUE(run != nullptr);
  EXPECT_EQ((int)run->steps.size(), 3);

  // 启动第一步 -> done -> 自动找下一步
  ws.mark_step_running(run->id, "step_research", clock.now_ms());
  ws.mark_step_done(run->id, "step_research", clock.now_ms());
  EXPECT_EQ(std::string(step_state_str(run->steps[0].state)), std::string("done"));

  // 第二步（并行，2 agents）标记失败 -> 重试 -> 跳过
  ws.mark_step_running(run->id, "step_implement", clock.now_ms());
  ws.mark_step_failed(run->id, "step_implement", "agent crashed", clock.now_ms());
  EXPECT_EQ(std::string(step_state_str(run->steps[1].state)), std::string("failed"));
  EXPECT_EQ(run->state, std::string("failed"));
  EXPECT_TRUE(ws.retry_step(run->id, "step_implement"));
  EXPECT_EQ(run->steps[1].attempts, 1);
  EXPECT_EQ(std::string(step_state_str(run->steps[1].state)), std::string("pending"));
  // 重试后又失败 -> 跳过（健壮性：失败不整体中断）
  ws.mark_step_running(run->id, "step_implement", clock.now_ms());
  ws.mark_step_failed(run->id, "step_implement", "again", clock.now_ms());
  EXPECT_TRUE(ws.skip_step(run->id, "step_implement"));
  ws.mark_step_running(run->id, "step_check", clock.now_ms());
  ws.mark_step_done(run->id, "step_check", clock.now_ms());
  ws.refresh_run_state(run->id);
  EXPECT_EQ(run->state, std::string("done"));

  // 定向引导到某步
  EXPECT_TRUE(ws.steer_step(run->id, "step_check", "focus on headers"));
  EXPECT_EQ(run->steps[2].guidance, std::string("focus on headers"));

  // 图结构：nodes 含 step + agent；edges 含 sequence + contains
  Json g = run->graph_json();
  ASSERT_TRUE(g["nodes"].is_array());
  ASSERT_TRUE(g["edges"].is_array());
  bool has_seq = false, has_contains = false;
  for (auto& e : g["edges"]) {
    if (e["kind"] == "sequence") has_seq = true;
    if (e["kind"] == "contains") has_contains = true;
  }
  EXPECT_TRUE(has_seq);
  EXPECT_TRUE(has_contains);
  // 按子代理反查运行
  run->steps[1].agents[0].subagentId = "sub_42";
  EXPECT_TRUE(ws.find_run_by_subagent("sub_42") == run);
}

// ---------- 双语 skill：zh/en 切换 ----------
FS_TEST(addon2_skill_bilingual) {
  SkillRegistry sk;
  // 浏览器控制 was removed: it was documentation for a driver that never
  // existed (see skill_registry.cpp). LSP is the remaining built-in.
  EXPECT_FALSE(sk.has("browser-control"));
  EXPECT_TRUE(sk.has("lsp"));

  FsResult zh = sk.load("lsp", ThinkLang::kZh);
  ASSERT_TRUE(zh.ok());
  EXPECT_EQ(zh.data["lang"].get<std::string>(), std::string("zh"));
  EXPECT_TRUE(zh.data["body"].get<std::string>().find("语言") != std::string::npos);

  FsResult en = sk.load("lsp", ThinkLang::kEn);
  ASSERT_TRUE(en.ok());
  EXPECT_EQ(en.data["lang"].get<std::string>(), std::string("en"));
  EXPECT_TRUE(en.data["body"].get<std::string>().find("LSP") != std::string::npos);

  // 未知 skill
  EXPECT_FALSE(sk.load("nope", ThinkLang::kZh).ok());

  // 列表
  Json list = sk.list_json(ThinkLang::kEn);
  ASSERT_TRUE(list.is_array());
  EXPECT_TRUE(list.size() >= 1);

  // 工具层：skill 工具走 ctx.thinking_lang
  ToolRegistry reg;
  register_skill_tools(&reg, &sk);
  ToolContext ctx;
  ctx.thinking_lang = "en";
  FsResult lr = reg.invoke(ctx, "load_skill", Json{{"name", "lsp"}});
  ASSERT_TRUE(lr.ok());
  EXPECT_EQ(lr.data["lang"].get<std::string>(), std::string("en"));
  FsResult ll = reg.invoke(ctx, "list_skills", Json::object());
  ASSERT_TRUE(ll.ok());
  EXPECT_TRUE(ll.data.is_array());
}

// ---------- todo 工具层（经 ToolContext.todo_op 转发）----------
FS_TEST(addon2_todo_tool_layer) {
  AgentHostDeps deps;
  FakeClockA2 clock;
  deps.clock = &clock;
  AgentHost host(std::move(deps));
  host.register_builtin_tools(nullptr);

  ToolContext ctx;
  ctx.sid = "s_9";
  ctx.todo_op = [&host](const Json& a, Json* out) -> FsResult {
    FsResult r = host.todo_op("s_9", a);
    if (r.ok() && out) *out = r.data;
    return r;
  };
  FsResult add = host.tools().invoke(ctx, "todo_add", Json{{"task", "write tests"}});
  ASSERT_TRUE(add.ok());
  // Compact result: ids/pending counts only, not the whole list.
  ASSERT_TRUE(add.data.contains("ids"));
  EXPECT_EQ((int)add.data["ids"].size(), 1);
  EXPECT_EQ((int)add.data["pending"].get<int>(), 1);
  EXPECT_FALSE(add.data.contains("items"));
  // Completing the last item does NOT delete the list: the backend keeps the
  // task list for the whole task, and the next round starts from it.
  int id = static_cast<int>(add.data["ids"][0].get<int>());
  FsResult chk = host.tools().invoke(ctx, "todo_check", Json{{"id", id}});
  ASSERT_TRUE(chk.ok());
  EXPECT_TRUE(chk.data["done"].get<bool>());
  EXPECT_EQ((int)chk.data["pending"].get<int>(), 0);
  FsResult lst = host.tools().invoke(ctx, "todo_list", Json::object());
  ASSERT_TRUE(lst.ok());
  EXPECT_EQ((int)lst.data["items"].size(), 1);
  // One call can add a batch; it returns all ids but not the list text.
  FsResult batch = host.tools().invoke(
      ctx, "todo_add", Json{{"tasks", Json::array({"a", "b", "c"})}});
  ASSERT_TRUE(batch.ok());
  EXPECT_EQ((int)batch.data["ids"].size(), 3);
  EXPECT_EQ((int)batch.data["pending"].get<int>(), 3);
  EXPECT_EQ((int)batch.data["total"].get<int>(), 4);
  // The model can drop an obsolete item itself, not only clear everything.
  FsResult add2 = host.tools().invoke(ctx, "todo_add", Json{{"task", "obsolete"}});
  ASSERT_TRUE(add2.ok());
  int id2 = static_cast<int>(add2.data["ids"][0].get<int>());
  FsResult ers = host.tools().invoke(ctx, "todo_erase", Json{{"id", id2}});
  ASSERT_TRUE(ers.ok());
  EXPECT_EQ((int)host.tools().invoke(ctx, "todo_list", Json::object()).data["items"].size(), 4);
  FsResult clr = host.tools().invoke(ctx, "todo_clear", Json::object());
  ASSERT_TRUE(clr.ok());
  EXPECT_EQ((int)host.tools().invoke(ctx, "todo_list", Json::object()).data["items"].size(), 0);
}

// A reopened session must show the WHOLE checklist, including items already
// marked done. Hydration used to be tested only for an empty store, so a
// completed item could be persisted correctly and still vanish on reopen.
FS_TEST(addon2_todo_hydrates_completed_items) {
  AgentHostDeps deps;
  deps.todo_load = [](const std::string& owner) -> Json {
    if (owner != "s_done") return Json::array();
    return Json::array({Json{{"id", 1}, {"task", "finished"}, {"done", true},
                             {"doneAt", 123}},
                        Json{{"id", 2}, {"task", "still open"}, {"done", false},
                             {"doneAt", 0}}});
  };
  AgentHost host(std::move(deps));
  Json items = host.todos_for("s_done");
  ASSERT_TRUE(items.is_array());
  EXPECT_EQ((int)items.size(), 2);
  EXPECT_EQ(items[0]["task"].get<std::string>(), std::string("finished"));
  EXPECT_TRUE(items[0]["done"].get<bool>());
  EXPECT_FALSE(items[1]["done"].get<bool>());
  // The next id continues after the restored maximum, not from 1.
  FsResult add = host.todo_op("s_done", Json{{"op", "add"}, {"task", "next"}});
  ASSERT_TRUE(add.ok());
  EXPECT_EQ(add.data["id"].get<int>(), 3);
}

// ---------- FsApi 层：skill.* / thinkingLang / browser-load ----------
FS_TEST(addon2_fsapi_skill_and_lang) {
  fstest::TempDir td("a2sk");
  FakeClockA2 clock(1000);
  FsApi api(ghc::filesystem::path(td.path()), &clock, nullptr);

  // 默认 auto；切 en 后 skill 正文为英文
  EXPECT_TRUE(api.set_thinking_lang("en").ok());
  FsResult sl = api.skill_list();
  ASSERT_TRUE(sl.ok());
  EXPECT_TRUE(sl.data.is_array());
  EXPECT_TRUE(sl.data.size() >= 1);

  FsResult ld = api.skill_load("lsp");
  ASSERT_TRUE(ld.ok());
  EXPECT_EQ(ld.data["lang"].get<std::string>(), std::string("en"));
  EXPECT_TRUE(ld.data["body"].get<std::string>().find("LSP") != std::string::npos);

  // 浏览器控制 has no loader any more: both the skill and its `browser.load`
  // route are gone, because no browser driver was ever implemented.
  EXPECT_FALSE(api.skill_load("browser-control").ok());

  // 未知 skill
  EXPECT_FALSE(api.skill_load("nope").ok());
}

// ---------- FsApi 层：工作流 start/get（图）/steer ----------
FS_TEST(addon2_fsapi_workflow) {
  fstest::TempDir td("a2wf");
  FakeClockA2 clock(1000);
  FsApi api(ghc::filesystem::path(td.path()), &clock, nullptr);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  // 定义 + 列表
  Json def = Json::object();
  def["name"] = "demo-flow";
  def["steps"] = Json::array({
      Json{{"id", "s1"}, {"name", "first"}, {"parallel", false},
           {"agents", Json::array()}},
      Json{{"id", "s2"}, {"name", "second"}, {"parallel", true},
           {"agents", Json::array()}}});
  EXPECT_TRUE(api.workflow_define(def).ok());
  FsResult wl = api.workflow_list();
  ASSERT_TRUE(wl.ok());
  bool found = false;
  for (auto& d : wl.data["defs"]) {
    if (d["name"] == "demo-flow") found = true;
  }
  EXPECT_TRUE(found);

  // 启动（无 agents 的步直接完成并推进到下一步）
  FsResult st = api.workflow_start(sid, "demo-flow");
  ASSERT_TRUE(st.ok());
  std::string runId = st.data["id"].get<std::string>();

  FsResult g = api.workflow_get(runId);
  ASSERT_TRUE(g.ok());
  EXPECT_TRUE(g.data.contains("graph"));
  EXPECT_TRUE(g.data["graph"]["nodes"].is_array());
  EXPECT_TRUE(g.data["graph"]["edges"].is_array());

  // 定向引导（步骤存在则 ok）
  EXPECT_TRUE(api.workflow_steer(runId, "s2", "hurry").ok());
  // 不存在的 run
  EXPECT_FALSE(api.workflow_get("run_nope").ok());
  EXPECT_FALSE(api.workflow_start(sid, "no-such-wf").ok());
}

// ---------- 模型规划的工作流：先提出，用户允许后才跑 ----------
FS_TEST(addon2_workflow_propose_needs_approval) {
  fstest::TempDir td("a2wfp");
  FakeClockA2 clock(1000);
  FsApi api(ghc::filesystem::path(td.path()), &clock, nullptr);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  Json plan = Json::object();
  plan["goal"] = "重构解析器";
  plan["steps"] = Json::array({
      Json{{"title", "调查现有实现"},
           {"task", "读 src/parser.cpp 并列出问题"}},
      Json{{"title", "写测试"}, {"task", "给 lexer 补单测"}}});
  FsResult n = api.workflow_propose(sid, plan);
  ASSERT_TRUE(n.ok());
  std::string run_id = n.data["id"].get<std::string>();
  // NOT started: the run waits for the user.
  EXPECT_EQ(n.data["state"].get<std::string>(), std::string("proposed"));

  FsResult runs = api.workflow_runs(sid);
  ASSERT_TRUE(runs.ok());
  ASSERT_TRUE(runs.data["runs"].is_array());
  ASSERT_TRUE(runs.data["runs"].size() == 1);
  EXPECT_EQ(runs.data["runs"][0]["state"].get<std::string>(),
            std::string("proposed"));
  EXPECT_EQ((int)runs.data["runs"][0]["steps"].size(), 2);
  // Each step is executed by a subagent.
  const Json& first_agents = runs.data["runs"][0]["steps"][0]["agents"];
  ASSERT_TRUE(first_agents.is_array() && first_agents.size() == 1);
  EXPECT_EQ(first_agents[0]["kind"].get<std::string>(),
            std::string("complete-step"));

  // Rejecting it ends it without running anything.
  FsResult rj = api.workflow_reject(run_id);
  ASSERT_TRUE(rj.ok());
  EXPECT_EQ(rj.data["state"].get<std::string>(), std::string("rejected"));

  // Approve only works on a PROPOSED run.
  FsResult bad = api.workflow_approve(run_id);
  EXPECT_FALSE(bad.ok());

  // A second proposal, then approve: it becomes running and the first step
  // starts (no agent kind override needed here - kind defaults to complete-step).
  FsResult n2 = api.workflow_propose(sid, plan);
  ASSERT_TRUE(n2.ok());
  FsResult ap = api.workflow_approve(n2.data["id"].get<std::string>());
  ASSERT_TRUE(ap.ok());
  EXPECT_EQ(ap.data["state"].get<std::string>(), std::string("running"));
}

// ---------- FsApi 层：中断-继续 + approve 门 ----------
FS_TEST(addon2_fsapi_continue_and_approve) {
  fstest::TempDir td("a2ct");
  FakeClockA2 clock(1000);
  FsApi api(ghc::filesystem::path(td.path()), &clock, nullptr);
  ghc::filesystem::path proj_dir = ghc::filesystem::path(td.path()) / "proj";
  ghc::filesystem::create_directories(proj_dir);
  FsResult pr = api.project_create(proj_dir.string());
  ASSERT_TRUE(pr.ok());
  std::string pid = pr.data["pid"].get<std::string>();
  FsResult sr = api.session_new(pid, proj_dir.string());
  ASSERT_TRUE(sr.ok());
  std::string sid = sr.data["sid"].get<std::string>();

  // 无 provider：call_model 回显模式，send 可跑通（turnDone）
  EXPECT_TRUE(api.session_send(sid, "hello", Json::object()).ok());
  // 未等待批准时 approve 返回 busy（状态机保护）
  EXPECT_FALSE(api.session_approve(sid, true).ok());
  // continue：无断点时按普通输入处理（不静默失败）
  EXPECT_TRUE(api.session_continue(sid, "keep going").ok());
  // 未知会话 continue 报 not-found
  EXPECT_FALSE(api.session_continue("s_nope", "x").ok());
}
