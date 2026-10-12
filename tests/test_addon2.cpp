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

// ---------- file.ls：基础（仅文件名）/ 详细（CSV）----------
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

  // 基础模式（默认）：只给文件名
  FsResult base = reg.invoke(ctx, "file.ls", Json::object());
  ASSERT_TRUE(base.ok());
  EXPECT_FALSE(base.data["detail"].get<bool>());
  std::string names = base.data["names"].get<std::string>();
  EXPECT_TRUE(names.find("alpha.txt") != std::string::npos);
  EXPECT_TRUE(names.find("sub") != std::string::npos);
  // 基础模式无 CSV
  EXPECT_FALSE(base.data.contains("entriesCsv"));

  // 详细模式：CSV 每行 name,type,date,size
  Json dargs = Json::object();
  dargs["detail"] = true;
  FsResult det = reg.invoke(ctx, "file.ls", dargs);
  ASSERT_TRUE(det.ok());
  EXPECT_TRUE(det.data["detail"].get<bool>());
  std::string csv = det.data["entriesCsv"].get<std::string>();
  EXPECT_TRUE(csv.find("alpha.txt,file,") != std::string::npos);
  EXPECT_TRUE(csv.find("sub,dir,") != std::string::npos);
}

// ---------- dir.check：子代理筛目录（复用 ls，带 requirement）----------
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
  EXPECT_FALSE(reg.invoke(ctx, "dir.check", Json::object()).ok());

  Json args = Json::object();
  args["requirement"] = "找所有 .hpp 头文件";
  FsResult r = reg.invoke(ctx, "dir.check", args);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.data["requirement"].get<std::string>(), std::string("找所有 .hpp 头文件"));
  EXPECT_TRUE(r.data.contains("entries"));
  EXPECT_TRUE(r.data.contains("hint"));
}

// ---------- file.regex：多文件替换 + $1 分组 + 无匹配不动 ----------
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
  FsResult r = reg.invoke(ctx, "file.regex", args);
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
  FsResult rb = reg.invoke(ctx, "file.regex", bad);
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
  FsResult r = reg.invoke(ctx, "file.write", wargs);
  EXPECT_FALSE(r.ok());

  // full 权限：允许项目外（逃逸检查仍在，绝对路径可用）
  ctx.permission = "full";
  FsResult r2 = reg.invoke(ctx, "file.write", wargs);
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
  loop.call_model = [&calls](const ModelCallRequest&, ModelCallResult* out) -> FsResult {
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

  // 写入计划
  Json plan = Json::object();
  plan["goal"] = "build feature X";
  plan["steps"] = Json::array({"a", "b"});
  EXPECT_TRUE(api.plan_update(sid, plan).ok());

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

  // reject：清掉计划
  EXPECT_TRUE(api.plan_review(sid, "reject", "not now").ok());
  FsResult pg = api.plan_get(sid);
  ASSERT_TRUE(pg.ok());
  EXPECT_TRUE(pg.data.is_null() || pg.data.empty() || !pg.data.contains("goal"));
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
  EXPECT_TRUE(sk.has("browser-control"));
  EXPECT_TRUE(sk.has("lsp"));

  FsResult zh = sk.load("browser-control", ThinkLang::kZh);
  ASSERT_TRUE(zh.ok());
  EXPECT_EQ(zh.data["lang"].get<std::string>(), std::string("zh"));
  EXPECT_TRUE(zh.data["body"].get<std::string>().find("浏览器") != std::string::npos);

  FsResult en = sk.load("browser-control", ThinkLang::kEn);
  ASSERT_TRUE(en.ok());
  EXPECT_EQ(en.data["lang"].get<std::string>(), std::string("en"));
  EXPECT_TRUE(en.data["body"].get<std::string>().find("Browser") != std::string::npos);

  // 未知 skill
  EXPECT_FALSE(sk.load("nope", ThinkLang::kZh).ok());

  // 列表
  Json list = sk.list_json(ThinkLang::kEn);
  ASSERT_TRUE(list.is_array());
  EXPECT_TRUE(list.size() >= 2);

  // 工具层：skill 工具走 ctx.thinking_lang
  ToolRegistry reg;
  register_skill_tools(&reg, &sk);
  ToolContext ctx;
  ctx.thinking_lang = "en";
  FsResult lr = reg.invoke(ctx, "tool.skill.load", Json{{"name", "lsp"}});
  ASSERT_TRUE(lr.ok());
  EXPECT_EQ(lr.data["lang"].get<std::string>(), std::string("en"));
  FsResult ll = reg.invoke(ctx, "tool.skill.list", Json::object());
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
  FsResult add = host.tools().invoke(ctx, "todo.add", Json{{"task", "write tests"}});
  ASSERT_TRUE(add.ok());
  EXPECT_EQ((int)add.data["items"].size(), 1);
  FsResult lst = host.tools().invoke(ctx, "todo.list", Json::object());
  ASSERT_TRUE(lst.ok());
  EXPECT_EQ((int)lst.data["items"].size(), 1);
  FsResult clr = host.tools().invoke(ctx, "todo.clear", Json::object());
  ASSERT_TRUE(clr.ok());
  EXPECT_EQ((int)host.tools().invoke(ctx, "todo.list", Json::object()).data["items"].size(), 0);
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
  EXPECT_TRUE(sl.data.size() >= 2);

  FsResult ld = api.skill_load("lsp");
  ASSERT_TRUE(ld.ok());
  EXPECT_EQ(ld.data["lang"].get<std::string>(), std::string("en"));
  EXPECT_TRUE(ld.data["body"].get<std::string>().find("LSP") != std::string::npos);

  // browser-load 加载口（addon2：按需加载，不直接提供）
  FsResult bl = api.browser_load();
  ASSERT_TRUE(bl.ok());
  EXPECT_EQ(bl.data["name"].get<std::string>(), std::string("browser-control"));

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
