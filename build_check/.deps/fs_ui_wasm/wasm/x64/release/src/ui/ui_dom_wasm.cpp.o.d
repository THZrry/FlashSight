{
    depfiles_format = "gcc",
    files = {
        [[src\ui\ui_dom_wasm.cpp]]
    },
    depfiles = "build_check\\.objs\\fs_ui_wasm\\wasm\\x64\\release\\src\\ui\\__cpp_ui_dom_wasm.cpp.cpp:   src\\ui\\ui_dom_wasm.cpp src\\ui\\ui_dom.hpp src\\common\\fs_json.hpp   3rdparty\\json\\nlohmann\\json.hpp 3rdparty\\json\\nlohmann\\..\\json.hpp\
",
    values = {
        [[C:\Program Files\emscripten\emscripten\emcc.exe]],
        {
            "-fvisibility=hidden",
            "-fvisibility-inlines-hidden",
            "-O3",
            "-std=c++17",
            "-Isrc",
            [[-Isrc\common]],
            [[-I3rdparty\filesystem-1.5.14\include]],
            [[-I3rdparty\json]],
            "-D_WIN32_WINNT=0x0A00",
            "-DWIN32_LEAN_AND_MEAN",
            "-Wall",
            "-Os",
            "-fvisibility=default",
            "-DNDEBUG"
        }
    }
}