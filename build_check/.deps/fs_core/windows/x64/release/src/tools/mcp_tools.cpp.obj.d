{
    depfiles_format = "gcc",
    files = {
        [[src\tools\mcp_tools.cpp]]
    },
    depfiles = "mcp_tools.o: src\\tools\\mcp_tools.cpp src\\tools\\mcp_tools.hpp  src\\tools\\tool_registry.hpp src\\common/fs_error.hpp  3rdparty\\json/nlohmann/json.hpp 3rdparty\\json/nlohmann/../json.hpp  src\\common/fs_json.hpp  3rdparty\\filesystem-1.5.14\\include/ghc/filesystem.hpp\
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