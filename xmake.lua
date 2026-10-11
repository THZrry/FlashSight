-- FlashSight build script
-- targets: fs_core (static) / fs_ui (static) / flashsight (bin) / fs_tests (bin)

set_project("FlashSight")
set_version("0.1.0")
set_xmakever("2.8.0")

add_rules("mode.debug", "mode.release")
set_languages("c++14")

-- common include dirs
includedirs = {
    "src",
    "src/common",
    "src/core",
    "src/storage",
    "src/provider",
    "src/agent",
    "src/tools",
    "3rdparty/filesystem-1.5.14/include",
    "3rdparty/json",
    "3rdparty/dtl-1.20",
    "3rdparty/md4c-master/src",
    "3rdparty/httplib",
    "3rdparty/mbedtls-3.6.4/include",
}

if is_plat("windows") then
    add_defines("_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN")
end

-- httplib 走 mbedTLS 后端（真网络实现；单测走 FakeNet 不受影响）
add_defines("CPPHTTPLIB_MBEDTLS_SUPPORT")

-- mbedtls 静态库（fs_core 的 fs_net_http.cpp 引用其符号；用 linkgroups 解决顺序）
MBEDTLS_LINKDIRS = {
    "3rdparty/mbedtls-3.6.4/build/library",
    "3rdparty/mbedtls-3.6.4/build/3rdparty/everest",
    "3rdparty/mbedtls-3.6.4/build/3rdparty/p256-m",
}
MBEDTLS_LINKS = { "mbedtls", "mbedx509", "mbedcrypto", "everest", "p256m" }

-- collect cpp files of dirs that exist (empty dirs are not errors)
function dir_files(...)
    local files = {}
    for _, d in ipairs({...}) do
        if os.isdir(d) then
            for _, f in ipairs(os.files(path.join(d, "**.cpp"))) do
                table.insert(files, f)
            end
        end
    end
    return files
end

target("fs_core")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files(dir_files("src/core", "src/agent", "src/tools",
                        "src/provider", "src/storage", "src/common"))
    add_files("3rdparty/md4c-master/src/md4c.c",
              "3rdparty/md4c-master/src/md4c-html.c",
              "3rdparty/md4c-master/src/entity.c")
    add_cflags("-Wall")
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "shell32", "crypt32", "bcrypt")
    end

target("fs_ui")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files(dir_files("src/ui"))
    add_cxxflags("-Wall")

-- mbedtls 静态库（fs_core 的 fs_net_http.cpp 引用其符号）
-- add_ldflags 追加在链接命令末尾（deps 库之后），顺序正确；路径用 projectdir 拼接
function mbedtls_link()
    local libdir = os.projectdir() .. "/3rdparty/mbedtls-3.6.4/build"
    add_ldflags(libdir .. "/library/libmbedtls.a")
    add_ldflags(libdir .. "/library/libmbedx509.a")
    add_ldflags(libdir .. "/library/libmbedcrypto.a")
    add_ldflags(libdir .. "/3rdparty/everest/libeverest.a")
    add_ldflags(libdir .. "/3rdparty/p256-m/libp256m.a")
    -- mbedcrypto 的 entropy_poll 引用 BCryptGenRandom（Windows）；放 mbedtls 之后
    add_ldflags("-lbcrypt", "-lcrypt32")
end

target("flashsight")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files("src/main.cpp")
    add_deps("fs_core", "fs_ui")
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "shell32", "crypt32", "bcrypt")
    end
    mbedtls_link()

target("fs_tests")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files(dir_files("tests"))
    add_deps("fs_core", "fs_ui")
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "shell32", "crypt32", "bcrypt")
    end
    mbedtls_link()