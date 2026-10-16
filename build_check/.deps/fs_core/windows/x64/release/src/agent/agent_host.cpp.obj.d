{
    depfiles_format = "gcc",
    files = {
        [[src\agent\agent_host.cpp]]
    },
    depfiles = "agent_host.o: src\\agent\\agent_host.cpp src\\agent\\agent_host.hpp  src\\agent\\agent_loop.hpp src\\common/fs_clock.hpp src\\common/fs_error.hpp  3rdparty\\json/nlohmann/json.hpp 3rdparty\\json/nlohmann/../json.hpp  src\\common/fs_json.hpp src\\agent\\history_builder.hpp  src\\tools/tool_registry.hpp src\\agent\\overfit.hpp  src\\tools/skill_registry.hpp src\\agent\\steering_queue.hpp  src\\agent\\subagent.hpp src\\tools/mcp_tools.hpp src\\agent\\todo_store.hpp  src\\agent\\wake_scheduler.hpp src\\tools/ask_tool.hpp  src\\tools/check_dir_tools.hpp src\\tools/file_tools.hpp  src\\tools/lsp_tools.hpp src\\tools/net_tools.hpp  src\\tools/regex_tools.hpp src\\tools/summarize_tool.hpp  src\\tools/term_tools.hpp src\\tools/todo_tools.hpp  src\\tools/vision_tools.hpp\
",
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
            "-DCPPHTTPLIB_MBEDTLS_SUPPORT",
            "-fexceptions",
            "-Wall",
            "-DNDEBUG"
        }
    }
}