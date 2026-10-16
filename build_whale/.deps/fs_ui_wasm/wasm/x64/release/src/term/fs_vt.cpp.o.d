{
    files = {
        [[src\term\fs_vt.cpp]]
    },
    depfiles_format = "gcc",
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
            "-DFS_UI_WHALEUI",
            "-Wall",
            "-Os",
            "-fvisibility=default",
            "-DNDEBUG"
        }
    },
    depfiles = "build_whale\\.objs\\fs_ui_wasm\\wasm\\x64\\release\\src\\term\\__cpp_fs_vt.cpp.cpp:   src\\term\\fs_vt.cpp src\\term\\fs_vt.hpp\
"
}