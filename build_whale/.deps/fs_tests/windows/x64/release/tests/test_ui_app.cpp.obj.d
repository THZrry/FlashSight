{
    files = {
        [[tests\test_ui_app.cpp]]
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
    depfiles = "build_whale\\.objs\\fs_tests\\windows\\x64\\release\\tests\\test_ui_app.cpp.obj:  tests\\test_ui_app.cpp tests\\test_framework.hpp src/ui/ui_app.hpp  src/ui/ui_backend.hpp src\\common/fs_error.hpp  3rdparty\\json/nlohmann/json.hpp 3rdparty\\json/nlohmann/../json.hpp  src\\common/fs_json.hpp src/ui/ui_components.hpp src/ui/ui_vdom.hpp  src/ui/ui_dom.hpp\
"
}