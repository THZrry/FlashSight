{
    depfiles_format = "gcc",
    files = {
        [[src\ui\ui_app.cpp]]
    },
    depfiles = "build_check\\.objs\\fs_ui_wasm\\wasm\\x64\\release\\src\\ui\\__cpp_ui_app.cpp.cpp:   src\\ui\\ui_app.cpp src\\ui\\ui_app.hpp src\\ui\\ui_backend.hpp   src\\common\\fs_error.hpp 3rdparty\\json\\nlohmann\\json.hpp   3rdparty\\json\\nlohmann\\..\\json.hpp src\\common\\fs_json.hpp   src\\ui\\ui_components.hpp src\\ui\\ui_vdom.hpp src\\ui\\ui_dom.hpp   src\\core\\settings_store.hpp   3rdparty\\filesystem-1.5.14\\include\\ghc\\filesystem.hpp   src\\common\\fs_types.hpp src\\common\\fs_i18n.hpp src\\term\\fs_vt.hpp\
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