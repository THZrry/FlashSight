// agent_context.cpp - context assembly (see header for the ordering rationale)
#include "agent_context.hpp"

#include <cstdio>

#include "fs_utf8.hpp"

namespace fs {

namespace {

bool zh(const ContextInput& in) { return in.lang == ContextLang::kZh; }

std::string join_lines(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += "\n";
    out += v[i];
  }
  return out;
}

// First sentence of a description, cut on a CHARACTER boundary.
//
// NOT find_first_of("。.\n"): that matches a SET OF BYTES, and '。' is
// E3 80 82 - the 0x80/0x82 bytes also occur as continuation bytes inside
// unrelated CJK characters. Cutting there produced a half-character, which is
// invalid UTF-8, and every later json::dump() threw type_error 316 (the whole
// turn died). Multi-byte delimiters must be searched as strings.
std::string first_sentence(const std::string& s) {
  size_t cut = s.size();
  size_t dot = s.find('.');
  if (dot != std::string::npos && dot < cut) cut = dot;
  size_t nl = s.find('\n');
  if (nl != std::string::npos && nl < cut) cut = nl;
  const std::string full_stop = "\xE3\x80\x82";  // 。
  size_t fs_pos = s.find(full_stop);
  if (fs_pos != std::string::npos && fs_pos < cut) cut = fs_pos;
  std::string out = s.substr(0, cut);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  return out;
}
// Defence in depth: drop any byte sequence that is not valid UTF-8. The prompt
// goes through json::dump() and the provider request, and one bad byte there
// kills the entire turn.
std::string sanitize_utf8(const std::string& s) { return utf8::sanitize(s); }

std::string kv(const std::string& k, const std::string& v) { return "- " + k + ": " + v; }

// Tool name -> one-line "when to use it". Only tools that are actually present
// get a line, so a disabled tool never shows up in the index.
std::string tool_hint_en(const std::string& name) {
  if (name == "read_file") return "read a file by lines; returns a hash for edit_file";
  if (name == "write_file") return "create a NEW file (overwrites; cannot merge)";
  if (name == "edit_file") return "replace a line range; hash + expect verify the edit";
  if (name == "list_dir") return "list a directory (names only unless detail=true)";
  if (name == "find_files") return "find files by name, or grep contents with grep=true";
  if (name == "remove_file") return "delete a file; no recycle bin, no second prompt";
  if (name == "file_regex") return "regex replace across files; returns before/after";
  if (name == "check_dir") return "let a subagent scan a directory against a requirement";
  if (name == "terminal_exec") return "run a command in the foreground and wait";
  if (name == "terminal_spawn") return "start a long-running command in the background";
  if (name == "terminal_poll") return "read new output from a background terminal";
  if (name == "terminal_kill") return "stop a terminal";
  if (name == "fetch_url") return "HTTP request (headers, timeout, size cap)";
  if (name == "search_web") return "web search";
  if (name == "ask") return "ask the user to choose; unavailable in auto mode";
  if (name == "todo_add") return "add a step to this turn's checklist";
  if (name == "todo_list") return "show the checklist";
  if (name == "todo_check") return "tick a step off";
  if (name == "todo_clear") return "drop the checklist";
  if (name == "summarize_session") return "record durable key points for later compaction";
  if (name == "load_skill") return "load a skill's full instructions";
  if (name == "list_skills") return "list available skills";
  if (name == "lsp") return "symbol-level refactors via a language server";
  if (name == "screenshot") return "capture the screen/window/region";
  if (name == "read_image") return "read a local image";
  if (name == "mcp_import") return "import tools from an external folder";
  if (name == "mcp_list") return "list imported tool packs";
  if (name == "mcp_enable" || name == "mcp_disable") return "toggle a tool pack";
  return "";
}

std::string tool_hint_zh(const std::string& name) {
  if (name == "read_file") return "按行读取；返回 hash 供 edit_file 校验";
  if (name == "write_file") return "新建文件（覆盖；不能合并）";
  if (name == "edit_file") return "按行号替换；hash + expect 双重校验";
  if (name == "list_dir") return "列目录（默认只给文件名）";
  if (name == "find_files") return "按文件名查找；grep=true 时搜内容";
  if (name == "remove_file") return "删除文件；不进回收站、不二次确认";
  if (name == "file_regex") return "跨文件正则替换；返回 before/after";
  if (name == "check_dir") return "让子代理按需求筛选目录";
  if (name == "terminal_exec") return "前台跑一条命令并等待结果";
  if (name == "terminal_spawn") return "后台启动长任务";
  if (name == "terminal_poll") return "读后台终端的新输出";
  if (name == "terminal_kill") return "停止终端";
  if (name == "fetch_url") return "HTTP 请求（头/超时/大小上限）";
  if (name == "search_web") return "网络搜索";
  if (name == "ask") return "请用户选择；自动模式不可用";
  if (name == "todo_add") return "给本轮加一个步骤";
  if (name == "todo_list") return "查看步骤清单";
  if (name == "todo_check") return "勾掉一个步骤";
  if (name == "todo_clear") return "清空步骤清单";
  if (name == "summarize_session") return "记录长期要点（压缩时保留）";
  if (name == "load_skill") return "加载某个 skill 的完整说明";
  if (name == "list_skills") return "列出可用 skill";
  if (name == "lsp") return "经语言服务器做符号级重构";
  if (name == "screenshot") return "截图（屏幕/窗口/区域）";
  if (name == "read_image") return "读取本地图片";
  if (name == "mcp_import") return "从外部文件夹导入工具包";
  if (name == "mcp_list") return "列出已导入的工具包";
  if (name == "mcp_enable" || name == "mcp_disable") return "启停工具包";
  return "";
}

}  // namespace

std::string AgentContext::tool_index(const ContextInput& in) {
  std::vector<std::string> lines;
  for (auto& tdef : in.tools) {
    if (!tdef.is_object()) continue;
    std::string name = jsonutil::get<std::string>(tdef, "name", "");
    if (name.empty()) continue;
    std::string hint = zh(in) ? tool_hint_zh(name) : tool_hint_en(name);
    if (hint.empty()) {
      // Unknown (MCP/plugin) tool: fall back to the first sentence of its own
      // description so the index never silently omits a capability.
      hint = first_sentence(jsonutil::get<std::string>(tdef, "description", ""));
    }
    if (hint.empty()) continue;
    lines.push_back("- " + name + ": " + hint);
  }
  return join_lines(lines);
}

std::string AgentContext::work_guidance(const ContextInput& in) {
  std::vector<std::string> out;

  if (zh(in)) {
    out.push_back("## 规划：什么时候用、怎么用");
    out.push_back(
        "- 任何需要 3 步以上、或要改多个文件的任务，先把步骤写进 todo（todo_add），"
        "每完成一项就 todo_check。清单对用户可见，是进度条而不是形式。");
    out.push_back(
        "- 步骤会变：发现新信息就增删/重排 todo，不要为了让旧清单成立而绕路。");
    out.push_back(
        "- 单步能做完的小事不要建清单；清单本身也是 token。");
    out.push_back(
        "- 三层任务，不要混：① 任务列表（todo_add/todo_check）是**本轮**的活；"
        "② 规划（plan_update）是一篇你负责的文档 `.flashsight/plans/<sid>.md`："
        "整个任务需要多轮时，先把目标/做法/验收写进文档，然后**每轮从文档里拆出"
        "当轮 todo**；做法变了就更新文档。文档没标记完成（plan_update status=done）"
        "就是工作没完，自动模式会继续；全部做完必须显式标记 done。"
        "③ 工作流（workflow_plan）是真正的大型流水线，要用户批准。"
        "规划不是另一个进度窗口，不要把它拆成 todo 之外的一套状态。");
    out.push_back(
        "- 先调查再动手：不确定的事实用 read_file / find_files / terminal 去确认，"
        "不要用\"应该\"\"通常\"代替验证。");
    if (in.allow_subagents) {
      // The core of the "delegate each step" pattern: the main conversation
      // keeps the plan and the summaries; each step's working context lives in
      // its own subagent transcript.
      out.push_back(
          "- **每一步单独派一个 complete-step**：清单建好后，逐项 subagent_spawn"
          "（kind=complete-step，task 写清该项的验收标准，step 填该项标题，"
          "todoId 填对应 id）。主会话只保留规划与摘要，逐步的工作过程在侧栏"
          "可查但不占主上下文。不要自己在主会话里把所有步骤做完。");
      out.push_back(
          "- 依赖关系：后一步依赖前一步结果时，等它返回摘要再派下一步；"
          "互不依赖的可以一次并行派出。");
    }
  } else {
    out.push_back("## Planning: when and how");
    out.push_back(
        "- For any task needing 3+ steps or touching several files, put the steps "
        "into todo (todo_add) and tick them off (todo_check) as you go. The list is "
        "visible to the user: it is a progress bar, not paperwork.");
    out.push_back(
        "- The plan is allowed to change. When you learn something new, add, remove "
        "or reorder steps instead of contorting the work to fit a stale list.");
    out.push_back(
        "- Do not create a list for a one-step task; the list itself costs tokens.");
    out.push_back(
        "- Three layers, never mixed: (1) the TASK LIST (todo_add/todo_check) is "
        "the work of THIS round; (2) the PLAN (plan_update) is a DOCUMENT you own "
        "at `.flashsight/plans/<sid>.md`: for a job that needs several rounds, "
        "write goal/approach/acceptance into it once, then derive each round's "
        "todo list from it; update the document when the approach changes. While "
        "it is not marked done (plan_update status=done) the job is unfinished and "
        "auto mode continues - so mark it done explicitly when everything is "
        "complete; (3) a WORKFLOW (workflow_plan) is a large pipeline and needs "
        "user approval. The plan is not a second progress widget: do not keep a "
        "step tree beside the checklist.");
    out.push_back(
        "- Investigate before acting. Confirm facts with read_file / find_files / "
        "terminal instead of substituting \"should be\" or \"usually\" for evidence.");
    if (in.allow_subagents) {
      out.push_back(
          "- **Delegate each step to its own complete-step subagent.** Once the "
          "checklist exists, spawn one per item (kind=complete-step, task = that "
          "item's acceptance criteria, step = the item title, todoId = its id). The "
          "main conversation keeps the plan and the summaries; each step's working "
          "context stays in its own transcript - readable in the side panel, not "
          "counted against this conversation. Do not do every step inline here.");
      out.push_back(
          "- Ordering: when a step needs the previous step's result, wait for its "
          "summary before spawning the next; independent steps can go out in "
          "parallel in one go.");
    }
  }

  if (in.allow_subagents) {
    if (zh(in)) {
      out.push_back("## 子代理：什么时候该派、派哪种");
      out.push_back(
          "- 判断标准是\"上下文成本\"：需要读很多文件才能得出结论、而你只需要结论时，"
          "派子代理；结论回来只占几行，原文不进主上下文。");
      out.push_back(
          "- 典型场景：全库找 bug / 找某个符号的所有引用 / 审查一批文件 / "
          "并行调研两个不相关的方向。");
      out.push_back(
          "- 任务要写成\"目标 + 已知信息 + 验收标准\"，子代理看不到你的对话历史。");
      out.push_back(
          "- 子代理只回摘要。需要细节时让它把结论写进文件，你再读那一段。");
      out.push_back("- 上限：" + std::to_string(in.max_subagent_parallel) +
                    " 个并行，嵌套深度 " + std::to_string(in.max_subagent_depth) + "。");
    } else {
      out.push_back("## Subagents: when to delegate, and which");
      out.push_back(
          "- The test is context cost: if answering requires reading a lot of files "
          "and you only need the conclusion, delegate. The conclusion costs a few "
          "lines; the sources never enter this conversation.");
      out.push_back(
          "- Typical cases: hunt a bug across the tree, find every reference to a "
          "symbol, review a batch of files, research two unrelated directions in "
          "parallel.");
      out.push_back(
          "- Write the task as goal + known facts + acceptance criteria. A subagent "
          "cannot see this conversation.");
      out.push_back(
          "- Subagents return summaries only. If you need detail, have the subagent "
          "write it to a file and read that range yourself.");
      out.push_back("- Limits: " + std::to_string(in.max_subagent_parallel) +
                    " parallel, nesting depth " + std::to_string(in.max_subagent_depth) + ".");
    }
  }

  if (in.allow_workflow) {
    if (zh(in)) {
      out.push_back("## 工作流：可复用的流水线");
      out.push_back(
          "- 一次性的多步任务用 todo；同一条流水线要反复跑（或每步都要单独重试/"
          "跳过/定向引导）时才用工作流。");
      out.push_back(
          "- 工作流的每一步是一组子代理，可并行；失败只影响那一步，"
          "可以重试或跳过，不会整体中断。");
    } else {
      out.push_back("## Workflows: reusable pipelines");
      out.push_back(
          "- Use todo for a one-off multi-step task. Use a workflow only when the "
          "same pipeline runs repeatedly, or when each step needs its own retry / "
          "skip / targeted steering.");
      out.push_back(
          "- A workflow step is a group of subagents and may run in parallel. A "
          "failure is contained to that step: retry or skip it, the run continues.");
    }
  }

  if (in.allow_ask) {
    out.push_back(zh(in)
        ? "## 不确定时\n- 需求含糊、有多种合理解释、或要做出用户才能决定的选择时，"
          "用 ask 让用户选（带推荐项）。不要替用户做取舍，也不要空转猜测。"
        : "## When unsure\n- If the request is ambiguous, has several reasonable "
          "readings, or needs a choice only the user can make, use ask with a "
          "recommended option. Do not pick for the user, and do not spin on guesses.");
  } else {
    out.push_back(zh(in)
        ? "## 自动模式\n- ask 不可用。遇到不确定的分支：选择可逆的那条，"
          "把假设写进回答里，并在需要用户拍板的地方停下等确认，不要武断下结论。"
        : "## Auto mode\n- ask is unavailable. When a branch is uncertain, take the "
          "reversible path, state the assumption in your reply, and stop for "
          "confirmation where the user must decide. Never assert a conclusion you "
          "have not verified.");
  }
  return join_lines(out);
}

std::string AgentContext::build_system(const ContextInput& in) {
  std::vector<std::string> s;

  // 1. Identity -------------------------------------------------------------
  if (in.role == "subagent") {
    std::string kind = in.subagent_kind.empty() ? "subagent" : in.subagent_kind;
    s.push_back(zh(in) ? ("You are " + in.agent_name + " 的子代理（" + kind + "）。")
                       : ("You are a subagent of " + in.agent_name + " (" + kind + ")."));
    s.push_back(zh(in)
        ? "你只负责交给你的那一步：完成后只返回摘要（结论 + 关键文件:行号 + 风险），"
          "不要复述过程，不要建议下一步，也不要向用户提问。"
        : "You own exactly one step. When done, return only a summary (conclusion + "
          "file:line evidence + risks). Do not narrate the process, do not propose "
          "next steps, and do not ask the user anything.");
    if (!in.subagent_step.empty()) {
      // A step subagent is handed ONE checklist item; planning the whole task
      // again would duplicate the parent's work and waste its own context.
      s.push_back(zh(in) ? ("清单项：" + in.subagent_step +
                            "\n只做这一项。做不了就如实说明原因，不要扩大范围。")
                         : ("Checklist item: " + in.subagent_step +
                            "\nDo only this item. If it cannot be done, say why. "
                            "Do not widen the scope - other steps belong to other "
                            "subagents."));
    }
    if (!in.subagent_task.empty()) {
      s.push_back((zh(in) ? "任务：\n" : "Task:\n") + in.subagent_task);
    }
  } else {
    s.push_back(zh(in) ? ("You are " + in.agent_name + ", a coding agent running on the "
                          "user's machine.")
                       : ("You are " + in.agent_name + ", a coding agent running on the "
                          "user's machine."));
    s.push_back(zh(in)
        ? "你直接操作用户的项目：读文件、改文件、跑命令。做完再说，不要只说\"将会\"。"
        : "You act on the user's project directly: read files, edit files, run "
          "commands. Do the work, then report - never announce what you \"will\" do.");
  }
  s.push_back("");

  // 2. Operating rules ------------------------------------------------------
  if (zh(in)) {
    s.push_back("## 工作方式");
    s.push_back("- 回答用用户的语言。先给结论，再给必要的证据；不复述用户已经知道的东西。");
    s.push_back("- 不谄媚：用户方案有问题就直接说，并给出替代方案。");
    s.push_back("- 不编造：没读过的文件不描述其内容，没跑过的命令不说它通过了。"
                "验证失败就如实报错并贴输出。");
    s.push_back("- 改代码要像周围的代码：同样的命名、缩进、注释密度。");
    s.push_back("- 注释只写代码本身表达不了的约束，不写\"这里做了什么\"。");
    s.push_back("- 工具调用之间不要空转：每次调用都要能推进任务，否则先想清楚再调。");
    s.push_back("- 结束时给一段能独立读懂的总结：做了什么、验证了什么、还剩什么。");
  } else {
    s.push_back("## How to work");
    s.push_back("- Answer in the user's language. Lead with the outcome, then the "
                "evidence that matters; do not restate what the user already knows.");
    s.push_back("- No flattery. If the user's approach is wrong, say so and offer an "
                "alternative.");
    s.push_back("- Never fabricate. Do not describe a file you have not read or claim a "
                "command passed when it did not. Report failures with the real output.");
    s.push_back("- Match the surrounding code: same naming, indentation, comment density.");
    s.push_back("- Comment only constraints the code cannot express, never \"what the "
                "next line does\".");
    s.push_back("- Every tool call must advance the task. If you are unsure, think "
                "before calling, not after.");
    s.push_back("- End with a summary that stands alone: what changed, what was "
                "verified, what is left.");
  }
  s.push_back("");

  // 3. Planning / subagents / workflow guidance -----------------------------
  s.push_back(work_guidance(in));
  s.push_back("");

  // 4. Tools ---------------------------------------------------------------
  std::string index = tool_index(in);
  if (!index.empty()) {
    s.push_back(zh(in) ? "## 工具（完整参数见 tools 参数）" : "## Tools (schemas are in the `tools` parameter)");
    s.push_back(index);
    if (!in.disabled_tools.empty()) {
      std::string names;
      for (auto& d : in.disabled_tools) {
        if (!d.is_string()) continue;
        if (!names.empty()) names += ", ";
        names += d.get<std::string>();
      }
      if (!names.empty()) {
        s.push_back(zh(in) ? ("- 已禁用（不要调用）：" + names)
                           : ("- Disabled (do not call): " + names));
      }
    }
    s.push_back("");
  }

  // 5. Skills / MCP --------------------------------------------------------
  bool have_skills = false;
  std::vector<std::string> sk;
  for (auto& k : in.skills) {
    if (!k.is_object()) continue;
    bool enabled = jsonutil::get<bool>(k, "enabled", true);
    if (!enabled) continue;
    std::string name = jsonutil::get<std::string>(k, "name", "");
    if (name.empty()) continue;
    if (!have_skills) {
      sk.push_back(zh(in) ? "## Skills（按需加载全文）" : "## Skills (load full text on demand)");
      sk.push_back(zh(in) ? "- 只给一行说明。要用法细节时调用 load_skill。"
                          : "- Only a one-line summary is shown. Call load_skill "
                            "for the full usage.");
      have_skills = true;
    }
    sk.push_back("- " + name + ": " + jsonutil::get<std::string>(k, "description", ""));
  }
  if (have_skills) {
    s.push_back(join_lines(sk));
    s.push_back("");
  }

  std::vector<std::string> mc;
  for (auto& m : in.mcp) {
    if (!m.is_object()) continue;
    std::string name = jsonutil::get<std::string>(m, "name", "");
    if (name.empty()) continue;
    bool enabled = jsonutil::get<bool>(m, "enabled", true);
    mc.push_back("- " + name + (enabled ? "" : (zh(in) ? "（已停用）" : " (disabled)")));
  }
  if (!mc.empty()) {
    s.push_back(zh(in) ? "## MCP 工具包" : "## MCP tool packs");
    s.push_back(join_lines(mc));
    s.push_back("");
  }

  // 6. Environment ---------------------------------------------------------
  std::vector<std::string> env;
  if (!in.cwd.empty()) env.push_back(kv("cwd", in.cwd));
  if (!in.os.empty()) env.push_back(kv("os", in.os));
  if (!in.shell.empty()) env.push_back(kv("shell", in.shell));
  if (!in.encoding.empty()) env.push_back(kv("terminal encoding", in.encoding));
  if (!in.model.empty()) env.push_back(kv("model", in.model));
  if (!in.thinking.empty()) env.push_back(kv("thinking", in.thinking));
  if (!in.git.empty()) {
    std::string branch = jsonutil::get<std::string>(in.git, "branch", "");
    if (!branch.empty()) {
      std::string line = branch;
      if (jsonutil::get<bool>(in.git, "dirty", false)) line += " (dirty)";
      int ahead = static_cast<int>(jsonutil::get_int(in.git, "ahead", 0));
      int behind = static_cast<int>(jsonutil::get_int(in.git, "behind", 0));
      if (ahead) line += " ahead " + std::to_string(ahead);
      if (behind) line += " behind " + std::to_string(behind);
      std::string op = jsonutil::get<std::string>(in.git, "operation", "");
      if (!op.empty()) line += " [" + op + " in progress]";
      env.push_back(kv("git", line));
    }
  }
  if (in.cli_tools.is_object() && !in.cli_tools.empty()) {
    std::vector<std::string> names;
    for (auto it = in.cli_tools.begin(); it != in.cli_tools.end(); ++it) {
      // Path is useful (the model may run it), the value is never a secret here.
      std::string v = it.value().is_string() ? it.value().get<std::string>() : "";
      if (v.empty()) continue;
      names.push_back(it.key() + "=" + v);
    }
    if (!names.empty()) {
      std::string line;
      for (size_t i = 0; i < names.size(); ++i) {
        if (i) line += "; ";
        line += names[i];
      }
      env.push_back(kv(zh(in) ? "可用 CLI" : "CLI tools", line));
    }
  }
  if (in.env_names.is_array() && !in.env_names.empty()) {
    std::string line;
    for (auto& n : in.env_names) {
      if (!n.is_string()) continue;
      if (!line.empty()) line += ", ";
      line += n.get<std::string>();
    }
    if (!line.empty()) {
      env.push_back(kv(zh(in) ? "环境变量（仅名字）" : "env vars (names only)", line));
    }
  }
  if (!in.permission.empty()) {
    std::string perm_text;
    if (in.permission == "approve") {
      perm_text = zh(in) ? "每次写/删/联网/终端都要用户批准"
                         : "every write/delete/network/terminal call asks the user";
    } else if (in.permission == "full") {
      perm_text = zh(in) ? "可读写整个文件系统，不再询问" : "full filesystem access, no prompts";
    } else {
      perm_text = zh(in) ? "项目内直接改，项目外和系统操作要批准"
                         : "project writes are allowed; outside the project and "
                           "system operations ask the user";
    }
    env.push_back(kv(zh(in) ? "文件权限" : "file permission", perm_text));
  }
  env.push_back(kv(zh(in) ? "模式" : "mode",
                   in.auto_mode ? (zh(in) ? "自动（一轮结束后若规划未闭合会继续）" : "auto (continues while the plan is open)")
                                : (zh(in) ? "手动（一轮结束即停）" : "manual (stops after each turn)")));
  if (!in.session_title.empty()) {
    env.push_back(kv(zh(in) ? "会话" : "session", in.session_title));
  }
  if (!env.empty()) {
    s.push_back(zh(in) ? "## 环境" : "## Environment");
    s.push_back(join_lines(env));
    s.push_back("");
  }

  // 7. Project instructions ------------------------------------------------
  if (!in.project_instructions.empty()) {
    s.push_back(zh(in) ? "## 项目约定（来自项目文件，优先级高于通用习惯）"
                       : "## Project instructions (from the repo; these outrank general habits)");
    if (!in.instructions_source.empty()) {
      s.push_back(zh(in) ? ("来源：" + in.instructions_source)
                         : ("Source: " + in.instructions_source));
    }
    s.push_back(in.project_instructions);
    s.push_back("");
  }

  // 8. Long-term memory ----------------------------------------------------
  if (in.memory.is_object() && !in.memory.empty()) {
    s.push_back(zh(in) ? "## 长期偏好（用户已确认）" : "## Long-term preferences (confirmed by the user)");
    for (auto it = in.memory.begin(); it != in.memory.end(); ++it) {
      std::string v = it.value().is_string() ? it.value().get<std::string>()
                                             : it.value().dump();
      s.push_back("- " + it.key() + ": " + v);
    }
    s.push_back("");
  }

  // 9. Thinking language ---------------------------------------------------
  // Only when the user explicitly configured one (doc: thinking language is not
  // injected when it is off/auto, otherwise the model is told to think in a
  // language the user did not choose).
  if (in.thinking_lang == "zh") {
    s.push_back("Think in Chinese. 用中文思考。");
  } else if (in.thinking_lang == "en") {
    s.push_back("Think in English.");
  }

  // 10. Output language ----------------------------------------------------
  // The settings page has always shown an "输出语言" selector; nothing read the
  // key, so picking a language changed nothing. It is a real instruction now.
  // Empty means the user left it at 不指定 - the model answers in the language of
  // the conversation, which is what "unspecified" should mean.
  if (in.output_lang == "zh") {
    s.push_back("Answer in Chinese. 用中文回答。");
  } else if (in.output_lang == "en") {
    s.push_back("Answer in English.");
  }

  // Trim trailing blank lines so the prefix is byte-stable.
  std::string out = sanitize_utf8(join_lines(s));
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

std::string AgentContext::build_runtime(const ContextInput& in, const RuntimeState& st) {
  std::vector<std::string> lines;
  if (st.now_ms > 0) {
    int64_t secs = st.now_ms / 1000;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    // Deterministic civil-from-days (Howard Hinnant), so no <ctime> locale mess.
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld %02lld:%02lld:%02lld UTC",
                  static_cast<long long>(y), static_cast<long long>(m),
                  static_cast<long long>(d), static_cast<long long>(rem / 3600),
                  static_cast<long long>((rem % 3600) / 60),
                  static_cast<long long>(rem % 60));
    lines.push_back(std::string(zh(in) ? "时间: " : "time: ") + buf);
  }

  // The plan is a DOCUMENT. The model does not need it re-printed every round
  // (it can be long) - what it needs is the fact that it exists, where it is,
  // and whether it is done, because that status is what auto mode follows.
  if (st.plan.is_object() && jsonutil::get<bool>(st.plan, "exists", false)) {
    std::string status = jsonutil::get<std::string>(st.plan, "status", "active");
    std::string path = jsonutil::get<std::string>(st.plan, "path", "");
    std::string content = jsonutil::get<std::string>(st.plan, "content", "");
    std::string where = path.empty() ? std::string(".flashsight/plans/<sid>.md") : path;
    if (zh(in)) {
      lines.push_back("规划文档: " + status + " (" + std::to_string(content.size()) +
                      " 字节) " + where);
      lines.push_back(status == "done"
                          ? "规划已标记完成，本轮可以收尾。"
                          : "规划未完成：用 plan_get 读全文，按文档拆出本轮 todo；"
                            "全部做完时 plan_update(status=\"done\")，"
                            "否则自动模式会继续。");
    } else {
      lines.push_back("plan document: " + status + " (" +
                      std::to_string(content.size()) + " bytes) " + where);
      lines.push_back(status == "done"
                          ? "The plan is marked done; wrap up this turn."
                          : "The plan is NOT done: read it (plan_get), derive this "
                            "round's todo list from it, and call "
                            "plan_update(status=\"done\") when everything is "
                            "complete - an active document keeps auto mode going.");
    }
  }

  if (st.todos.is_array() && !st.todos.empty()) {
    int pending = 0;
    std::vector<std::string> tl;
    for (auto& t : st.todos) {
      if (!t.is_object()) continue;
      bool done = jsonutil::get<bool>(t, "done", false);
      if (!done) ++pending;
      tl.push_back(std::string(done ? "[x] " : "[ ] ") +
                   jsonutil::get<std::string>(t, "task", ""));
    }
    lines.push_back((zh(in) ? "本轮清单（剩 " : "checklist (") + std::to_string(pending) +
                    (zh(in) ? " 项）:" : " open):"));
    for (auto& x : tl) lines.push_back(x);
  }

  if (st.subagents.is_array() && !st.subagents.empty()) {
    lines.push_back(zh(in) ? "子代理:" : "subagents:");
    for (auto& s : st.subagents) {
      if (!s.is_object()) continue;
      std::string id = jsonutil::get<std::string>(s, "id", "");
      std::string kind = jsonutil::get<std::string>(s, "kind", "");
      std::string status = jsonutil::get<std::string>(s, "status", "");
      std::string summary = jsonutil::get<std::string>(s, "summary", "");
      std::string line = "- " + id + " (" + kind + "): " + status;
      if (!summary.empty()) line += " - " + summary;
      lines.push_back(line);
    }
  }

  if (st.pending_wakes.is_array() && !st.pending_wakes.empty()) {
    lines.push_back(zh(in) ? "待唤醒事件:" : "pending wakes:");
    for (auto& w : st.pending_wakes) {
      if (!w.is_object()) continue;
      lines.push_back("- " + jsonutil::get<std::string>(w, "source", "") + " " +
                      jsonutil::get<std::string>(w, "ref", ""));
    }
  }

  if (st.context_limit > 0 && st.context_tokens > 0) {
    int pct = static_cast<int>(st.context_tokens * 100 / st.context_limit);
    lines.push_back((zh(in) ? "上下文水位: " : "context: ") + std::to_string(pct) + "% (" +
                    std::to_string(st.context_tokens) + "/" +
                    std::to_string(st.context_limit) + " tokens)");
  }

  if (!st.turn_note.empty()) lines.push_back(st.turn_note);

  if (lines.empty()) return std::string();
  return sanitize_utf8(join_lines(lines));
}

std::string AgentContext::wrap_user_turn(const std::string& runtime,                                         const std::string& text) {
  if (runtime.empty()) return text;
  return "<runtime>\n" + runtime + "\n</runtime>\n\n<user>\n" + text + "\n</user>";
}

std::string AgentContext::unwrap_user_text(const std::string& content) {
  size_t open = content.find("<user>\n");
  if (open == std::string::npos) return content;
  size_t start = open + 7;
  size_t close = content.rfind("</user>");
  if (close == std::string::npos || close < start) return content;
  std::string out = content.substr(start, close - start);
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

}  // namespace fs
