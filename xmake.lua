-- FlashSight build script
-- native: fs_core / fs_ui / flashsight / fs_tests
-- wasm  : fs_ui_wasm (src/ui + minimal common, via xmake emcc toolchain)

set_project("FlashSight")
set_version("0.1.0")
set_xmakever("2.8.0")

add_rules("mode.debug", "mode.release")
set_languages("c++14")

-- include dirs (shared)
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
    "3rdparty/qjs-regex",
}

-- wasm needs a smaller include set (no mbedtls/httplib/md4c)
wasm_includedirs = {
    "src",
    "src/common",
    "3rdparty/filesystem-1.5.14/include",
    "3rdparty/json",
}

if is_plat("windows") then
    add_defines("_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN")
end

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

-- ============================ native ============================
if not is_plat("wasm") then

add_defines("CPPHTTPLIB_MBEDTLS_SUPPORT")

-- mbedtls 静态库（fs_core 的 fs_net_http.cpp 引用其符号）
function mbedtls_link()
    local libdir = os.projectdir() .. "/3rdparty/mbedtls-3.6.4/build"
    add_ldflags(libdir .. "/library/libmbedtls.a")
    add_ldflags(libdir .. "/library/libmbedx509.a")
    add_ldflags(libdir .. "/library/libmbedcrypto.a")
    add_ldflags(libdir .. "/3rdparty/everest/libeverest.a")
    add_ldflags(libdir .. "/3rdparty/p256-m/libp256m.a")
    add_ldflags("-lbcrypt", "-lcrypt32")
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
    add_files("3rdparty/qjs-regex/libregexp.c", "3rdparty/qjs-regex/libunicode.c")
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

end  -- not wasm

-- ============================ wasm ============================
if is_plat("wasm") then

target("fs_ui_wasm")
    set_kind("binary")
    set_languages("c++14")
    set_toolchains("emcc")
    add_includedirs(wasm_includedirs)
    add_files("src/ui/ui_dom_null.cpp", "src/ui/ui_dom_browser.cpp",
              "src/ui/ui_backend.cpp", "src/ui/ui_widgets.cpp",
              "src/ui/ui_app.cpp", "src/ui/ui_main_wasm.cpp")
    add_files("src/common/fs_json.cpp", "src/common/fs_log.cpp")
    -- xmake 默认注入 -fvisibility=hidden，会隐藏 extern "C" 入口；改回 default
    add_cxflags("-fvisibility=default")
    add_cxxflags("-Wall", "-Os")
    set_targetdir("web")
    set_basename("fs_ui")
    add_ldflags("-sALLOW_MEMORY_GROWTH=1", "-sDISABLE_EXCEPTION_CATCHING=0",
                "--no-entry", {force = true})
    add_ldflags("-sEXPORTED_FUNCTIONS=_fs_ui_start,_fs_ui_show,_fs_ui_fire_event,_fs_ui_fire,_malloc,_free",
                {force = true})
    add_ldflags("-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8",
                {force = true})
    -- 把 DOM 适配层打进 fs_ui.js（wasm 运行时自带 glue）
    add_ldflags("--post-js", os.projectdir() .. "/web/glue.js", {force = true})
    after_build(function (target)
        print("wasm built: " .. target:targetfile())
        print("next: python scripts/gen_assets.py  (embed web/ into src/ui/ui_assets_gen.hpp)")
    end)

end  -- wasm
