// ui_api.cpp - method dispatch from UI to FsApi.
#include "ui_api.hpp"

#include "fs_path.hpp"
#include "core/fs_api.hpp"

namespace fs {
namespace ui {

namespace {

std::string s(const Json& j, const char* k, const std::string& def = "") {
  return jsonutil::get<std::string>(j, k, def);
}
int64_t i(const Json& j, const char* k, int64_t def = 0) {
  return jsonutil::get_int(j, k, def);
}
bool b(const Json& j, const char* k, bool def = false) {
  return jsonutil::get<bool>(j, k, def);
}

}  // namespace

UiApi::UiApi(FsApi* api) : api_(api) {
  // ---- project.* ----
  reg("project.list", [api](const Json&) { return api->project_list(); });
  reg("project.open", [api](const Json& a) { return api->project_open(s(a, "path")); });
  reg("project.create", [api](const Json& a) { return api->project_create(s(a, "path")); });
  reg("project.remove", [api](const Json& a) { return api->project_remove(s(a, "pid")); });
  reg("project.get", [api](const Json& a) { return api->project_get(s(a, "pid")); });
  // native folder dialog (the browser cannot hand out OS paths)
  reg("project.pickFolder", [api](const Json&) { return api->project_pick_folder(); });
  // 选文件（不是目录）：驱动路径 / CLI 工具路径用得上
  reg("dialog.pickFile", [api](const Json& a) {
    return api->pick_file(s(a, "filterDesc"), s(a, "filterSpec"));
  });
  // resolve a dropped folder name to a path (browsers hide absolute paths)
  reg("project.resolveDrop", [api](const Json& a) {
    return api->project_resolve_drop(s(a, "name"));
  });
  // open the project folder in the OS file manager
  reg("project.reveal", [api](const Json& a) { return api->project_reveal(s(a, "pid")); });
  // A markdown link in the transcript: the UI reports the click, the backend
  // decides where it goes (browser / preview panel / refused).
  reg("link.open", [api](const Json& a) {
    return api->open_link(s(a, "sid"), s(a, "href"));
  });
  // side-panel file tree: list a dir / read a file inside the project
  reg("project.files", [api](const Json& a) {
    return api->project_files(s(a, "pid"), s(a, "path"));
  });
  // Composer path completion ("/" and "./" fragments).
  reg("complete.path", [api](const Json& a) {
    return api->complete_path(s(a, "sid"), s(a, "typed"));
  });
  reg("project.readFile", [api](const Json& a) {
    return api->project_read_file(s(a, "pid"), s(a, "path"));
  });

  // ---- session.* ----
  reg("session.new", [api](const Json& a) {
    return api->session_new(s(a, "pid"), s(a, "cwd"));
  });
  reg("session.open", [api](const Json& a) { return api->session_open(s(a, "sid")); });
  reg("session.close", [api](const Json& a) { return api->session_close(s(a, "sid")); });
  reg("session.list", [api](const Json& a) { return api->session_list(s(a, "pid")); });
  reg("session.delete", [api](const Json& a) {
    return api->session_delete(s(a, "sid"));
  });
  reg("session.rename", [api](const Json& a) {
    return api->session_rename(s(a, "sid"), s(a, "title"));
  });
  reg("session.aiTitle", [api](const Json& a) {
    return api->session_ai_title(s(a, "sid"));
  });
  reg("session.get", [api](const Json& a) { return api->session_get(s(a, "sid")); });
  reg("session.info", [api](const Json& a) { return api->session_info(s(a, "sid")); });
  // Paged history: the UI loads the newest page and prepends older ones.
  reg("session.turns", [api](const Json& a) {
    return api->session_turns(s(a, "sid"), static_cast<int>(i(a, "before", -1)),
                              static_cast<int>(i(a, "limit", 30)));
  });
  reg("session.fork", [api](const Json& a) {
    return api->session_fork(s(a, "sid"), s(a, "turnId"));
  });
  reg("session.rollback", [api](const Json& a) {
    return api->session_rollback(s(a, "sid"), s(a, "turnId"));
  });
  reg("session.forget", [api](const Json& a) {
    return api->session_forget(s(a, "sid"), s(a, "scope"));
  });
  reg("session.send", [api](const Json& a) {
    return api->session_send(s(a, "sid"), s(a, "text"),
                             a.contains("attachments") ? a["attachments"] : Json::array(),
                             s(a, "model"), s(a, "thinking"));
  });
  reg("session.append", [api](const Json& a) {
    return api->session_append(s(a, "sid"), s(a, "text"));
  });
  reg("session.suspend", [api](const Json& a) { return api->session_suspend(s(a, "sid")); });
  reg("session.resume", [api](const Json& a) { return api->session_resume(s(a, "sid")); });
  reg("session.cancelWake", [api](const Json& a) {
    return api->session_cancel_wake(s(a, "sid"));
  });
  reg("session.schedule", [api](const Json& a) {
    return api->session_schedule(s(a, "sid"), (int64_t)i(a, "delayMs", 0),
                                 s(a, "note"));
  });
  reg("session.pendingWakes", [api](const Json& a) {
    return api->session_pending_wakes(s(a, "sid"));
  });
  reg("session.loopState", [api](const Json& a) {
    return api->session_loop_state(s(a, "sid"));
  });
  // 编辑并重发最后一条：只截断转录，不回退文件改动
  reg("session.trimHistory", [api](const Json& a) {
    return api->session_trim_history(s(a, "sid"), s(a, "turnId"));
  });
  reg("session.continue", [api](const Json& a) {
    return api->session_continue(s(a, "sid"), s(a, "guidance"));
  });
  reg("session.approve", [api](const Json& a) {
    return api->session_approve(s(a, "sid"), b(a, "yes", true));
  });
  reg("session.compress", [api](const Json& a) {
    return api->session_compress(s(a, "sid"));
  });
  reg("session.context", [api](const Json& a) {
    return api->session_context(s(a, "sid"));
  });
  reg("session.liveTurn", [api](const Json& a) {
    return api->session_live_turn(s(a, "sid"));
  });
  reg("session.setModel", [api](const Json& a) {
    return api->session_set_model(s(a, "sid"), s(a, "model"));
  });
  reg("session.setThinking", [api](const Json& a) {
    return api->session_set_thinking(s(a, "sid"), s(a, "thinking"));
  });
  reg("session.setPermission", [api](const Json& a) {
    return api->session_set_permission(s(a, "sid"), s(a, "permission"));
  });
  reg("session.setPermissionConfirm", [api](const Json& a) {
    return api->session_set_permission_confirm(s(a, "sid"), s(a, "permission"),
                                               s(a, "token"));
  });
  reg("session.requestPermissionToken", [api](const Json& a) {
    return api->session_request_permission_token(s(a, "sid"), s(a, "newPermission"));
  });
  reg("session.setAutoMode", [api](const Json& a) {
    return api->session_set_auto_mode(s(a, "sid"), b(a, "autoMode"));
  });
  reg("session.setEnterSend", [api](const Json& a) {
    return api->session_set_enter_send(s(a, "sid"), s(a, "enterSend"));
  });
  reg("session.refreshConfig", [api](const Json& a) {
    return api->session_refresh_config(s(a, "sid"));
  });

  // ---- terminal.* ----
  reg("terminal.list", [api](const Json& a) { return api->terminal_list(s(a, "sid")); });
  reg("terminal.show", [api](const Json& a) { return api->terminal_show(s(a, "tid")); });
  // 用户终端：起"设置里的那个 shell"，不需要用户输入命令
  reg("terminal.spawn", [api](const Json& a) {
    return api->terminal_spawn(s(a, "sid"), s(a, "shell"), s(a, "label"),
                               s(a, "cwd"),
                               static_cast<int>(i(a, "cols", 100)),
                               static_cast<int>(i(a, "rows", 30)));
  });
  // 交互式终端的按键（UI 已按 VT 编码；含控制字符，不能走普通文本输入）
  reg("terminal.write", [api](const Json& a) {
    return api->terminal_write(s(a, "tid"), s(a, "text"));
  });
  // 窗口大小变化（全屏程序据此排版）
  reg("terminal.resize", [api](const Json& a) {
    return api->terminal_resize(s(a, "tid"), static_cast<int>(i(a, "cols", 100)),
                                static_cast<int>(i(a, "rows", 30)));
  });
  // 屏幕网格（终端真正画的东西）
  reg("terminal.grid", [api](const Json& a) { return api->terminal_grid(s(a, "tid")); });
  // Search the SAVED history (output chain + typed input). The screen in memory
  // only covers the visible window; this is what makes a restored terminal
  // searchable.
  reg("terminal.search", [api](const Json& a) {
    return api->terminal_search(s(a, "tid"), s(a, "q"),
                                static_cast<int>(jsonutil::get_int(a, "limit", 200)));
  });
  // 改名字（终端跑着也能改）
  reg("terminal.rename", [api](const Json& a) {
    return api->terminal_rename(s(a, "tid"), s(a, "label"));
  });
  reg("terminal.kill", [api](const Json& a) { return api->terminal_kill(s(a, "tid")); });
  reg("terminal.setEncoding", [api](const Json& a) {
    return api->terminal_set_encoding(s(a, "tid"), s(a, "enc"));
  });
  // 浏览器端：UI 定时器驱动一次非阻塞轮询（桌面端有后台线程，此调用无害）
  reg("terminal.tick", [api](const Json&) {
    api->terminal_tick();
    return FsResult::success();
  });

  // ---- git.* (side panel: status / diff / stage / commit) ----
  // Everything is addressed by pid; the UI never builds a repository path.
  reg("git.status", [api](const Json& a) { return api->git_status(s(a, "pid")); });
  reg("git.diff", [api](const Json& a) {
    return api->git_diff(s(a, "pid"), s(a, "path"), b(a, "staged", false));
  });
  reg("git.showHead", [api](const Json& a) {
    return api->git_show_head(s(a, "pid"), s(a, "path"));
  });
  reg("git.log", [api](const Json& a) {
    return api->git_log(s(a, "pid"), static_cast<int>(i(a, "maxCount", 30)));
  });
  reg("git.branches", [api](const Json& a) { return api->git_branches(s(a, "pid")); });
  reg("git.stage", [api](const Json& a) {
    return api->git_stage(s(a, "pid"), a.contains("paths") ? a["paths"] : Json::array());
  });
  reg("git.unstage", [api](const Json& a) {
    return api->git_unstage(s(a, "pid"), a.contains("paths") ? a["paths"] : Json::array());
  });
  reg("git.stageAll", [api](const Json& a) { return api->git_stage_all(s(a, "pid")); });
  reg("git.commit", [api](const Json& a) {
    return api->git_commit(s(a, "pid"), s(a, "message"), b(a, "amend", false));
  });
  reg("git.discard", [api](const Json& a) {
    return api->git_discard(s(a, "pid"), a.contains("paths") ? a["paths"] : Json::array());
  });

  // ---- command.* (composer "命令" menu) ----
  reg("command.list", [api](const Json& a) { return api->command_list(s(a, "sid")); });
  reg("command.run", [api](const Json& a) {
    return api->command_run(s(a, "sid"), s(a, "name"));
  });

  // ---- usage.* ----
  reg("usage.query", [api](const Json& a) {
    return api->usage_query(a.contains("filter") ? a["filter"] : Json::object());
  });
  // Per-session usage for the side panel: the same query with a sid filter, so
  // "what has this conversation cost" has a real backend answer instead of a
  // hard-coded number in the UI.
  reg("usage.session", [api](const Json& a) {
    return api->usage_query(Json{{"sid", s(a, "sid")}});
  });
  reg("usage.exportCsv", [api](const Json& a) {
    return api->usage_export_csv(a.contains("filter") ? a["filter"] : Json::object(),
                                 s(a, "path"));
  });

  // ---- settings.* ----
  reg("settings.get", [api](const Json& a) {
    return api->settings_get(s(a, "key"), s(a, "pid"));
  });
  reg("settings.set", [api](const Json& a) {
    return api->settings_set(s(a, "key"), a.contains("value") ? a["value"] : Json(),
                             s(a, "pid"));
  });
  reg("server.status", [api](const Json&) { return api->server_status(); });
  // Auth probe: the page asks this before showing anything. Loopback is always
  // authenticated, so the desktop/browser-on-the-same-machine case never sees the
  // password gate.
  reg("server.authRequired", [api](const Json&) {
    return api->server_auth_required();
  });
  reg("server.login", [api](const Json& a) {
    return api->server_login(s(a, "password"));
  });
  reg("settings.unset", [api](const Json& a) {
    return api->settings_unset(s(a, "key"), s(a, "pid"));
  });
  reg("settings.dump", [api](const Json& a) { return api->settings_dump(s(a, "pid")); });
  reg("settings.detectShells", [api](const Json&) { return api->settings_detect_shells(); });
  reg("environment.detectCliTools", [api](const Json&) {
    return api->environment_detect_cli_tools();
  });
  reg("settings.setShell", [api](const Json& a) {
    return api->settings_set_shell(s(a, "name"));
  });
  reg("settings.setIgnoreGlobal", [api](const Json& a) {
    return api->settings_set_ignore_global(s(a, "pid"), b(a, "ignore"));
  });

  // ---- plan.* ----
  reg("plan.get", [api](const Json& a) { return api->plan_get(s(a, "sid")); });
  reg("plan.update", [api](const Json& a) {
    return api->plan_update(s(a, "sid"),
                            a.contains("plan") ? a["plan"] : Json::object());
  });
  reg("plan.review", [api](const Json& a) {
    return api->plan_review(s(a, "sid"), s(a, "verdict"), s(a, "comment"));
  });

  // ---- workflow.* ----
  reg("workflow.define", [api](const Json& a) {
    return api->workflow_define(a.contains("def") ? a["def"] : Json::object());
  });
  reg("workflow.list", [api](const Json&) { return api->workflow_list(); });
  reg("workflow.runs", [api](const Json& a) {
    return api->workflow_runs(s(a, "sid"));
  });
  reg("workflow.start", [api](const Json& a) {
    return api->workflow_start(s(a, "sid"), s(a, "name"));
  });
  // 模型规划的工作流：propose 落成待允许的实例，approve/reject 由侧栏按钮触发
  reg("workflow.propose", [api](const Json& a) {
    return api->workflow_propose(s(a, "sid"), a);
  });
  reg("workflow.approve", [api](const Json& a) {
    return api->workflow_approve(s(a, "runId"));
  });
  reg("workflow.reject", [api](const Json& a) {
    return api->workflow_reject(s(a, "runId"));
  });
  reg("workflow.get", [api](const Json& a) { return api->workflow_get(s(a, "runId")); });
  reg("workflow.retryStep", [api](const Json& a) {
    return api->workflow_retry_step(s(a, "runId"), s(a, "stepId"));
  });
  reg("workflow.skipStep", [api](const Json& a) {
    return api->workflow_skip_step(s(a, "runId"), s(a, "stepId"));
  });
  reg("workflow.steer", [api](const Json& a) {
    return api->workflow_steer(s(a, "runId"), s(a, "stepId"), s(a, "text"));
  });

  // ---- lock.* ----
  reg("lock.acquire", [api](const Json& a) { return api->lock_acquire(s(a, "sid")); });
  reg("lock.release", [api](const Json& a) { return api->lock_release(s(a, "sid")); });
  reg("lock.holder", [api](const Json& a) { return api->lock_holder(s(a, "pid")); });

  // ---- tool.* / ask / subagent / steering ----
  reg("tool.invoke", [api](const Json& a) {
    return api->tool_invoke(s(a, "sid"), s(a, "name"),
                            a.contains("args") ? a["args"] : Json::object());
  });
  reg("tool.askAnswer", [api](const Json& a) {
    return api->ask_answer(s(a, "sid"), static_cast<int>(i(a, "askId")), s(a, "selection"));
  });
  reg("subagent.spawn", [api](const Json& a) {
    return api->subagent_spawn(s(a, "sid"), s(a, "kind"), s(a, "task"), s(a, "model"),
                               static_cast<int>(i(a, "depth", 0)), i(a, "timeoutMs", 0));
  });
  reg("subagent.cancel", [api](const Json& a) { return api->subagent_cancel(s(a, "id")); });
  reg("subagent.list", [api](const Json& a) { return api->subagent_list(s(a, "sid")); });
  // One agent's full transcript: the detail tab fetches it on open, so the list
  // itself stays small (a transcript repeats the whole system prompt per message).
  // `sid` lets the backend recover a record from disk when the in-memory one is
  // gone (restart / prune) - without it an older transcript's row opened nothing.
  reg("subagent.detail", [api](const Json& a) {
    return api->subagent_detail_in(s(a, "sid"), s(a, "id"));
  });
  reg("subagent.steer", [api](const Json& a) {
    return api->subagent_steer(s(a, "id"), s(a, "text"));
  });
  reg("steering.push", [api](const Json& a) {
    return api->steering_push(s(a, "sid"), s(a, "kind"), s(a, "text"));
  });
  reg("steering.list", [api](const Json& a) { return api->steering_list(s(a, "sid")); });
  // Continue a FINISHED subagent with more work (same id, same context).
  reg("subagent.followUp", [api](const Json& a) {
    return api->subagent_follow_up(s(a, "id"), s(a, "text"));
  });

  // ---- provider.* ----
  reg("provider.listProviders", [api](const Json&) { return api->provider_list_providers(); });
  reg("provider.addProvider", [api](const Json& a) {
    return api->provider_add_provider(a.contains("conn") ? a["conn"] : Json::object());
  });
  reg("provider.removeProvider", [api](const Json& a) {
    return api->provider_remove_provider(s(a, "providerId"));
  });
  reg("provider.getProvider", [api](const Json& a) {
    return api->provider_get_provider(s(a, "providerId"));
  });
  reg("provider.templates", [api](const Json&) { return api->provider_templates(); });
  reg("provider.listModels", [api](const Json& a) {
    return api->provider_list_models(s(a, "providerId"));
  });
  reg("provider.testConnection", [api](const Json& a) {
    return api->provider_test_connection(s(a, "providerId"));
  });
  // 余额/额度：一家一行，status 告诉 UI 是查到了、不支持还是获取失败
  reg("provider.balances", [api](const Json&) { return api->provider_balances(); });

  // ---- model.*（逐模型设置：能力/档位/重试/启用/图标）----
  reg("model.list", [api](const Json& a) { return api->model_list(s(a, "providerId")); });
  reg("model.save", [api](const Json& a) {
    return api->model_save(s(a, "providerId"),
                           a.contains("model") ? a["model"] : Json::object());
  });
  reg("model.remove", [api](const Json& a) {
    return api->model_remove(s(a, "providerId"), s(a, "modelId"));
  });
  reg("model.setEnabled", [api](const Json& a) {
    return api->model_set_enabled(s(a, "providerId"), s(a, "modelId"),
                                  b(a, "enabled", true));
  });
  reg("model.test", [api](const Json& a) {
    return api->model_test(s(a, "providerId"), s(a, "modelId"));
  });
  reg("model.allEnabled", [api](const Json&) { return api->model_all_enabled(); });

  // ---- mcp.*（外部工具/技能包管理）----
  reg("mcp.list", [api](const Json&) { return api->mcp_list(); });
  reg("mcp.import", [api](const Json& a) { return api->mcp_import(s(a, "dir")); });
  reg("mcp.add", [api](const Json& a) {
    return api->mcp_add(s(a, "name"), s(a, "dir"), b(a, "enabled", true));
  });
  reg("mcp.setEnabled", [api](const Json& a) {
    return api->mcp_set_enabled(s(a, "name"), b(a, "enabled", true));
  });
  reg("mcp.remove", [api](const Json& a) { return api->mcp_remove(s(a, "name")); });
  reg("mcp.detect", [api](const Json&) { return api->mcp_detect(); });

  // ---- skill / misc ----
  reg("skill.list", [api](const Json&) { return api->skill_list(); });
  reg("skill.load", [api](const Json& a) { return api->skill_load(s(a, "name")); });
  reg("skill.import", [api](const Json& a) { return api->skill_import(s(a, "dir")); });
  reg("skill.create", [api](const Json& a) {
    return api->skill_create(s(a, "name"), s(a, "title"), s(a, "description"),
                             s(a, "body"));
  });
  reg("skill.setEnabled", [api](const Json& a) {
    return api->skill_set_enabled(s(a, "name"), b(a, "enabled", true));
  });
  reg("skill.remove", [api](const Json& a) { return api->skill_remove(s(a, "name")); });
  // Skill detection is its own route: sharing mcp.detect meant the Skill page
  // could only ever find MCP entries.
  reg("skill.detect", [api](const Json&) { return api->skill_detect(); });
  reg("thinking.setLang", [api](const Json& a) {
    return api->set_thinking_lang(s(a, "lang", "auto"));
  });
  // available subagent presets (settings > subagents)
  reg("subagent.presets", [](const Json&) {
    Json arr = Json::array();
    std::vector<PresetAgent> presets = builtin_preset_agents();
    for (size_t i = 0; i < presets.size(); ++i) {
      Json j = Json::object();
      j["name"] = presets[i].name;
      j["description"] = presets[i].description;
      j["parallel"] = presets[i].parallel;
      j["blocking"] = presets[i].blocking;
      j["readOnly"] = presets[i].read_only;
      j["enabled"] = presets[i].enabled;
      arr.push_back(j);
    }
    return FsResult::success(arr);
  });
}

void UiApi::reg(const std::string& name, std::function<FsResult(const Json&)> fn) {
  methods_[name] = std::move(fn);
}

bool UiApi::has(const std::string& method) const {
  return methods_.find(method) != methods_.end();
}

std::vector<std::string> UiApi::methods() const {
  std::vector<std::string> out;
  for (std::map<std::string, std::function<FsResult(const Json&)> >::const_iterator it =
           methods_.begin();
       it != methods_.end(); ++it) {
    out.push_back(it->first);
  }
  return out;
}

FsResult UiApi::call(const std::string& method, const Json& args) {
  std::map<std::string, std::function<FsResult(const Json&)> >::iterator it =
      methods_.find(method);
  if (it == methods_.end()) {
    return FsResult::error(ErrCode::kNotFound, "unknown method: " + method);
  }
  // ONE lock for the whole UI->backend boundary.
  //
  // The desktop build runs calls on worker threads while a turn runs on its own,
  // so two calls can be inside FsApi at the same time; FsApi holds maps and is
  // not thread-safe. Guarding here (rather than in 130 methods) means every
  // route is covered by construction, including ones added later.
  //
  // The lock is recursive because routes call each other.
  //
  // Two routes are EXCLUDED because they BLOCK for a long time:
  //   * session.send  - the whole turn (provider round trip + every tool call).
  //     It takes the lock itself around its short prelude only.
  //   * tool.invoke   - arbitrary tool code (a terminal command can take a
  //     minute) and must not hold the backend lock while it runs.
  // Holding the lock for either would stall every other call for minutes, which
  // is the freeze this design exists to prevent.
  if (!api_ || method == "session.send" || method == "tool.invoke") {
    return it->second(args);
  }
  std::lock_guard<std::recursive_mutex> lk(api_->mutex());
  return it->second(args);
}

}  // namespace ui
}  // namespace fs
