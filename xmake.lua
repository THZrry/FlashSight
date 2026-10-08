-- FlashSight build script
-- targets: fs_core (static) / fs_ui (static) / flashsight (bin) / fs_tests (bin)

set_project("FlashSight")
set_version("0.1.0")
set_xmakever("2.8.0")

add_rules("mode.debug", "mode.release")
set_languages("c++14")

-- common 3rdparty include dirs
includedirs = {
    "3rdparty/filesystem-1.5.14/include",
    "3rdparty/json",
    "3rdparty/dtl-1.20",
    "3rdparty/md4c-master/src",
    "3rdparty/httplib",
}

if is_plat("windows") then
    add_defines("_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN")
end

target("fs_core")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files("src/core/**.cpp", "src/agent/**.cpp", "src/tools/**.cpp",
              "src/provider/**.cpp", "src/storage/**.cpp", "src/common/**.cpp")
    add_files("3rdparty/md4c-master/src/md4c.c",
              "3rdparty/md4c-master/src/md4c-html.c",
              "3rdparty/md4c-master/src/entity.c")
    add_cflags("-Wall")
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "shell32")
    end

target("fs_ui")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files("src/ui/**.cpp")
    add_cxxflags("-Wall")

target("flashsight")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files("src/main.cpp")
    add_deps("fs_core", "fs_ui")
    add_cxxflags("-Wall")

target("fs_tests")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files("tests/**.cpp")
    add_deps("fs_core", "fs_ui")
    add_cxxflags("-Wall")