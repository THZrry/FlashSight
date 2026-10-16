{
    files = {
        [[tests\test_addon2.cpp]]
    },
    depfiles_format = "gcc",
    values = {
        [[C:\Program Files\mingw64\bin\x86_64-w64-mingw32-g++]],
        {
            "-m64",
            "-fvisibility=hidden",
            "-fvisibility-inlines-hidden",
            "-O3",
            "-std=c++14",
            "-Isrc",
            [[-Isrc\common]],
            [[-Isrc\core]],
            [[-Isrc\storage]],
            [[-Isrc\provider]],
            [[-Isrc\agent]],
            [[-Isrc\tools]],
            [[-I3rdparty\filesystem-1.5.14\include]],
            [[-I3rdparty\json]],
            [[-I3rdparty\dtl-1.20]],
            [[-I3rdparty\md4c-master\src]],
            [[-I3rdparty\httplib]],
            [[-I3rdparty\mbedtls-3.6.4\include]],
            [[-I3rdparty\qjs-regex]],
            [[-I3rdparty\reproc\include]],
            [[-I3rdparty\whaleui\include]],
            "-D_WIN32_WINNT=0x0A00",
            "-DWIN32_LEAN_AND_MEAN",
            "-DFS_UI_WHALEUI",
            "-fexceptions",
            "-Wall",
            "-DNDEBUG"
        }
    },
    depfiles = "build_whale\\.objs\\fs_tests\\windows\\x64\\release\\tests\\test_addon2.cpp.obj:  tests\\test_addon2.cpp  3rdparty\\filesystem-1.5.14\\include/ghc/filesystem.hpp  tests\\test_framework.hpp src/agent/agent_loop.hpp  src\\common/fs_clock.hpp src\\common/fs_error.hpp  3rdparty\\json/nlohmann/json.hpp 3rdparty\\json/nlohmann/../json.hpp  src\\common/fs_json.hpp src/agent/history_builder.hpp  src\\tools/tool_registry.hpp src/agent/todo_store.hpp  src/common/fs_path.hpp src/core/fs_api.hpp src\\common/fs_net_http.hpp  src\\common/fs_net.hpp src\\common/fs_random.hpp src/core/deny_list.hpp  src/core/lock_arbiter.hpp src/core/plan_store.hpp  src/core/project_manager.hpp src/core/settings_store.hpp  src\\common/fs_types.hpp src/storage/project_store.hpp  src/storage/session_store.hpp src/storage/fs_store.hpp  src/core/session_manager.hpp src/storage/fs_patch.hpp  src/core/shell_detect.hpp src/core/terminal_manager.hpp  src/core/conpty.hpp src/core/terminal_registry.hpp src/term/fs_vt.hpp  src/core/usage_store.hpp src/core/workflow.hpp src/agent/agent_host.hpp  src/agent/overfit.hpp src\\tools/skill_registry.hpp  src/agent/steering_queue.hpp src/agent/subagent.hpp  src\\tools/mcp_tools.hpp src/agent/wake_scheduler.hpp  src/provider/model_table.hpp src/provider/provider_types.hpp  src/provider/provider_client.hpp src/provider/provider_runtime.hpp  src/provider/provider_store.hpp src/tools/check_dir_tools.hpp  src/tools/file_tools.hpp src/tools/noise_detect.hpp  src/tools/regex_tools.hpp src/tools/todo_tools.hpp\
"
}