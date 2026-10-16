{
    depfiles_format = "gcc",
    files = {
        [[src\core\session_manager.cpp]]
    },
    depfiles = "session_manager.o: src\\core\\session_manager.cpp  src\\core\\session_manager.hpp  3rdparty\\filesystem-1.5.14\\include/ghc/filesystem.hpp  src\\common/fs_clock.hpp src\\common/fs_error.hpp  3rdparty\\json/nlohmann/json.hpp 3rdparty\\json/nlohmann/../json.hpp  src\\common/fs_json.hpp src\\common/fs_random.hpp  src\\core\\lock_arbiter.hpp src\\core\\project_manager.hpp  src\\core\\settings_store.hpp src\\common/fs_types.hpp  src/storage/project_store.hpp src/storage/session_store.hpp  src/storage/fs_store.hpp src/storage/fs_patch.hpp src\\common/fs_path.hpp  src\\common/fs_serialize.hpp\
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