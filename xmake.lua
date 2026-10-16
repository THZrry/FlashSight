-- FlashSight build script
-- One command builds everything:
--   * fs_ui_wasm : the UI, compiled to WebAssembly (per-target platform override, emcc)
--   * fs_ui      : native UI lib (same sources + null/Whale DOM adapter)
--   * flashsight : native exe with the web assets embedded (served over local HTTP)
--   * fs_tests   : unit tests
-- The wasm target is a build dependency of the native UI lib; its output is packed
-- into src/ui/ui_assets_gen.hpp by a Lua hook (no external scripts, no bat, no
-- hardcoded paths). See rules/addon3.md.

set_project("FlashSight")
set_version("0.1.0")
set_xmakever("2.8.0")

add_rules("mode.debug", "mode.release")
set_languages("c++14")
-- Build targets sequentially in dependency order. The wasm bundle is a build
-- input to the native UI (its bytes are embedded in ui_assets_gen.hpp), and
-- xmake's default parallel-across-targets scheduling starts fs_ui's compile
-- before fs_ui_wasm has produced web/fs_ui.js. Serializing makes add_deps a
-- real ordering edge for compilation, not just linking.
set_policy("build.across_targets_in_parallel", false)

-- include dirs (native)
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
    "3rdparty/reproc/include",
    "3rdparty/whaleui/include",
}

-- WhaleUI integration switch. OFF by default: the desktop UI is written and
-- wired (ui_dom_whale.cpp / ui_main_whale.cpp, guarded by FS_UI_WHALEUI) but not
-- compiled in until WhaleUI itself is finished. See doc-internal/ui-whaleui.md.
option("whaleui")
    set_default(false)
    set_showmenu(true)
    set_description("Build the WhaleUI desktop UI (native window) instead of the wasm/HTTP one")
option_end()

-- path to a WhaleUI build (the directory holding libwhaleui-*.a); only used with
-- --whaleui=y. Override with `xmake f --whaleui=y --whaleui_libdir=...`
option("whaleui_libdir")
    set_default("C:/aaa/WhaleUI/build/mingw/x86_64/release")
    set_showmenu(true)
    set_description("Directory containing WhaleUI's built static library")
option_end()

local whaleui_on = has_config("whaleui")

-- Where WhaleUI's already-installed xrepo dependencies live (lexbor, utf8proc,
-- SDL3_image, SDL3). WhaleUI built them from source; we link the same copies
-- instead of declaring add_requires(), because re-installing lexbor needs a VS
-- toolchain this machine does not have. Override if your xmake package cache is
-- elsewhere:  xmake f --whaleui=y --whaleui_pkgdir=D:/xmake/packages
option("whaleui_pkgdir")
    set_default(path.join(os.getenv("LOCALAPPDATA") or "", ".xmake/packages"))
    set_showmenu(true)
    set_description("xmake package cache holding WhaleUI's dependencies")
option_end()

-- wasm needs a smaller include set (no mbedtls/httplib/md4c/reproc)
wasm_includedirs = {
    "src",
    "src/common",
    "3rdparty/filesystem-1.5.14/include",
    "3rdparty/json",
}

if is_plat("windows") then
    add_defines("_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN")
end

-- Shared config: the WhaleUI desktop UI. Only the native build can use it (the
-- wasm target is the browser backend), and ui_dom_whale/ui_main_whale are
-- #ifdef'd so defining the macro is what turns the code on.
if whaleui_on and not is_plat("wasm") then
    add_defines("FS_UI_WHALEUI")
    local libdir = get_config("whaleui_libdir")
    add_linkdirs(libdir)
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

-- UI sources shared by the native and wasm builds
ui_shared = {
    "src/ui/ui_dom_null.cpp",
    "src/ui/ui_vdom.cpp",
    "src/ui/ui_components.cpp",
    "src/ui/ui_app.cpp",
    "src/ui/ui_backend.cpp",
    "src/common/fs_json.cpp",
    "src/common/fs_log.cpp",
    "src/common/fs_i18n.cpp",
    -- terminal engine (VT parser + screen grid): shared so the browser renders
    -- the same grid the native build does. It also compiles into fs_core (see
    -- dir_files below) so the backend can hold a Screen per terminal - hence
    -- NOT in fs_ui's own file list, which would duplicate the symbols.
    "src/term/fs_vt.cpp",
}

-- Web assets the packer embeds. They are INPUTS to the after_build packing step,
-- so editing style.css must invalidate the target: otherwise the change never
-- reaches ui_assets_gen.hpp and the app keeps serving the previous build's CSS
-- (which is exactly how a CSS fix silently "does not work"). Declaring them as
-- headers is the cheap way to get that dependency edge - headers are not
-- compiled, only tracked, and a change re-runs the target's after_build.
-- fs_ui.js / fs_ui.wasm are deliberately NOT here: they are this target's own
-- outputs, so listing them as inputs would be circular.
web_assets = {
    "web/index.html",
    "web/style.css",
    "web/icons/fa-subset.css",
    "web/icons/fa-solid-900.woff2",
}

-- ==========================================================================
-- wasm UI target: built as a dependency of fs_ui (per-target platform).
-- ==========================================================================
-- emcc on PATH? (search PATH entries for emcc / emcc.exe)
local function has_emcc()
    local path = os.getenv("PATH") or ""
    for entry in path:gmatch("[^;]+") do
        if entry ~= "" then
            local base = entry
            local last = base:sub(-1)
            if last ~= "\\" and last ~= "/" then base = base .. "\\" end
            if os.isfile(base .. "emcc") or os.isfile(base .. "emcc.exe") then
                return true
            end
        end
    end
    return false
end
local emcc_ok = has_emcc()

if emcc_ok and not whaleui_on then
target("fs_ui_wasm")
    set_kind("binary")
    set_plat("wasm")
    set_toolchains("emcc")
    -- emscripten/val.h (the DOM adapter) requires C++17; the native build stays C++14.
    set_languages("c++17")
    add_includedirs(wasm_includedirs)
    add_files(ui_shared)
    add_files("src/ui/ui_dom_wasm.cpp", "src/ui/ui_main_wasm.cpp",
              "src/ui/ui_http_backend.cpp")
    -- asset inputs (see web_assets): keeps the packed header in sync
    add_headerfiles(web_assets)
    -- xmake injects -fvisibility=hidden by default, which hides the extern "C" entries
    add_cxflags("-fvisibility=default")
    add_cxxflags("-Wall", "-Os")
    set_targetdir("web")
    set_basename("fs_ui")
    -- Output fs_ui.js (+ fs_ui.wasm); the default .html would be a single
    -- self-contained file with no separate js/wasm for the packer to embed.
    set_extension(".js")
    add_ldflags("-lembind", "-sALLOW_MEMORY_GROWTH=1", "-sDISABLE_EXCEPTION_CATCHING=0",
                "--no-entry", {force = true})
    add_ldflags("-sEXPORTED_FUNCTIONS=_fs_ui_start,_fs_ui_show,_fs_ui_fire_event,_fs_ui_fire,_fs_ui_drop,_fs_ui_split,_fs_ui_vsplit,_fs_ui_key,_fs_ui_key_input,_fs_ui_cell_size,_fs_ui_file_pick,_fs_ui_scroll_pin,_fs_api_done,_fs_ui_backend_debug,_fs_ui_debug,_malloc,_free",
                {force = true})
    add_ldflags("-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8",
                {force = true})
    -- Nothing to pack here: the packing runs in fs_ui's before_build (see there
    -- for why after_build here was too late to invalidate fs_ui's sources).
end  -- fs_ui_wasm

-- ============================ native ============================
if not is_plat("wasm") then

-- mbedtls static libs (fs_net_http.cpp references their symbols)
function mbedtls_link()
    local libdir = os.projectdir() .. "/3rdparty/mbedtls-3.6.4/build"
    add_ldflags(libdir .. "/library/libmbedtls.a")
    add_ldflags(libdir .. "/library/libmbedx509.a")
    add_ldflags(libdir .. "/library/libmbedcrypto.a")
    add_ldflags(libdir .. "/3rdparty/everest/libeverest.a")
    add_ldflags(libdir .. "/3rdparty/p256-m/libp256m.a")
    add_ldflags("-lbcrypt", "-lcrypt32")
end

-- WhaleUI static library + its own dependencies (only with --whaleui=y).
--
-- WhaleUI itself links SDL3 / SDL3_ttf from prebuilt packages under its own
-- 3rdparty/ and lexbor / utf8proc / SDL3_image from xrepo. All of those are
-- already installed on this machine (WhaleUI built against them), so this finds
-- the static libs in the xmake package cache and links them directly rather
-- than declaring add_requires() - re-installing lexbor would need a VS
-- toolchain, which this machine does not have.
--
-- The library is attached to fs_ui (the target whose objects reference the
-- whaleui_* symbols) so xmake emits it AFTER fs_ui.lib on the link line: on the
-- exe target it would be emitted FIRST and every whaleui_* reference would fail
-- to resolve (static-lib link order). Call inside a target scope.
function whaleui_link()
    local libdir = get_config("whaleui_libdir")
    local whaleui_dir = "C:/aaa/WhaleUI"
    local pkg = get_config("whaleui_pkgdir")

    -- find <name>/<version>/<hash>/lib/*.a in the xmake package cache
    local function find_pkg_lib(name, libfile)
        local base = path.join(pkg, name:sub(1, 1), name)
        for _, ver in ipairs(os.dirs(path.join(base, "*"))) do
            for _, h in ipairs(os.dirs(path.join(ver, "*"))) do
                local lib = path.join(h, "lib", libfile)
                if os.isfile(lib) then
                    return lib
                end
            end
        end
        return nil
    end

    add_linkdirs(libdir)
    add_links("whaleui-full")
    -- SDL3 / SDL3_ttf: prebuilt import libs (they load their DLLs at run time)
    if os.isdir(path.join(whaleui_dir, "3rdparty/sdl3/lib")) then
        add_linkdirs(path.join(whaleui_dir, "3rdparty/sdl3/lib"))
        add_links("SDL3")
    end
    if os.isdir(path.join(whaleui_dir, "3rdparty/sdl3_ttf/lib")) then
        add_linkdirs(path.join(whaleui_dir, "3rdparty/sdl3_ttf/lib"))
        add_links("SDL3_ttf")
    end
    -- static xrepo dependencies, in dependency order
    local statics = {
        {name = "lexbor", lib = "liblexbor_static.a"},
        {name = "utf8proc", lib = "libutf8proc.a"},
        {name = "libsdl3_image", lib = "libSDL3_image.a"},
    }
    for _, e in ipairs(statics) do
        local lib = find_pkg_lib(e.name, e.lib)
        if lib then
            add_ldflags(lib)
        else
            print("[whaleui] warning: " .. e.name .. " not found under " .. pkg)
        end
    end
    if is_plat("windows") then
        add_syslinks("gdi32", "winmm", "imm32", "oleaut32", "version", "advapi32",
                     "setupapi")
    end
end

-- Copy WhaleUI's runtime DLLs and the web/ assets next to the exe. The static
-- lib still loads SDL3/SDL3_ttf at run time, WhaleUI's D3D12 path loads
-- dxcompiler/dxil, and the desktop UI reads web/style.css + the icon subset
-- through the VFS (a relative path, so it resolves from the working directory).
-- Call inside a target scope; it installs the after_build hook itself.
function whaleui_copy_dlls()
    local libdir = get_config("whaleui_libdir")
    local whaleui_dir = "C:/aaa/WhaleUI"
    local dirs = {
        libdir,
        path.join(whaleui_dir, "3rdparty/sdl3/bin"),
        path.join(whaleui_dir, "3rdparty/sdl3_ttf/bin"),
        path.join(whaleui_dir, "3rdparty/dxc/bin"),
    }
    after_build(function (target)
        local outdir = path.directory(target:targetfile())
        for _, dll in ipairs({"SDL3.dll", "SDL3_ttf.dll", "dxcompiler.dll", "dxil.dll"}) do
            for _, d in ipairs(dirs) do
                local src = path.join(d, dll)
                if os.isfile(src) then
                    os.cp(src, path.join(outdir, dll))
                    break
                end
            end
        end
        -- the UI's stylesheet + icon font, in the layout the app asks for
        local webdir = path.join(outdir, "web")
        os.mkdir(webdir)
        os.mkdir(path.join(webdir, "icons"))
        for _, rel in ipairs({"style.css", "index.html", "icons/fa-subset.css",
                              "icons/fa-solid-900.woff2"}) do
            local src = path.join(os.projectdir(), "web", rel)
            if os.isfile(src) then
                os.cp(src, path.join(webdir, rel))
            end
        end
    end)
end

target("fs_core")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_defines("CPPHTTPLIB_MBEDTLS_SUPPORT")
    add_files(dir_files("src/core", "src/agent", "src/tools",
                        "src/provider", "src/storage", "src/common", "src/term"))
    add_files("3rdparty/md4c-master/src/md4c.c",
              "3rdparty/md4c-master/src/md4c-html.c",
              "3rdparty/md4c-master/src/entity.c")
    add_files("3rdparty/qjs-regex/libregexp.c", "3rdparty/qjs-regex/libunicode.c")
    -- reproc: platform files are selected per OS (posix files don't compile on Windows)
    local reproc_common = {
        "3rdparty/reproc/src/drain.c",
        "3rdparty/reproc/src/options.c",
        "3rdparty/reproc/src/redirect.c",
        "3rdparty/reproc/src/reproc.c",
        "3rdparty/reproc/src/run.c",
        "3rdparty/reproc/src/strv.c",
    }
    local reproc_plat = {}
    if is_plat("windows") then
        reproc_plat = {
            "3rdparty/reproc/src/process.windows.c",
            "3rdparty/reproc/src/clock.windows.c",
            "3rdparty/reproc/src/error.windows.c",
            "3rdparty/reproc/src/handle.windows.c",
            "3rdparty/reproc/src/init.windows.c",
            "3rdparty/reproc/src/pipe.windows.c",
            "3rdparty/reproc/src/redirect.windows.c",
            "3rdparty/reproc/src/utf.windows.c",
        }
    else
        reproc_plat = {
            "3rdparty/reproc/src/process.posix.c",
            "3rdparty/reproc/src/clock.posix.c",
            "3rdparty/reproc/src/error.posix.c",
            "3rdparty/reproc/src/handle.posix.c",
            "3rdparty/reproc/src/init.posix.c",
            "3rdparty/reproc/src/pipe.posix.c",
            "3rdparty/reproc/src/redirect.posix.c",
            "3rdparty/reproc/src/utf.posix.c",
        }
    end
    add_files(reproc_common)
    add_files(reproc_plat)
    add_cflags("-Wall")
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "shell32", "ole32", "uuid", "crypt32", "bcrypt")
    end

target("fs_ui")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files(dir_files("src/ui"))
    if whaleui_on then
        -- The WhaleUI desktop UI needs no HTTP server, and ui_server.cpp pulls in
        -- ui_assets_gen.hpp (which only the wasm bundle produces) - so drop it
        -- from this build. The browser path is unaffected (default config).
        remove_files("src/ui/ui_server.cpp")
        whaleui_link()
    end
    add_cxxflags("-Wall")
    if emcc_ok and not whaleui_on then
        -- fs_ui_wasm produces web/fs_ui.js + fs_ui.wasm, which the packer embeds.
        add_deps("fs_ui_wasm", {link = false})
        -- Pack the assets BEFORE fs_ui's own sources are checked for staleness:
        -- a header regenerated in fs_ui_wasm's after_build is already "not newer"
        -- by then, so a style.css edit never reached this target.
        before_build(function (target)
            local root = os.projectdir()
            local items = {
                {name = "index.html", mime = "text/html; charset=utf-8"},
                {name = "style.css",  mime = "text/css; charset=utf-8"},
                {name = "fs_ui.js",   mime = "application/javascript; charset=utf-8"},
                {name = "fs_ui.wasm", mime = "application/wasm"},
                {name = "icons/fa-subset.css", mime = "text/css; charset=utf-8"},
                {name = "icons/fa-solid-900.woff2", mime = "font/woff2"},
            }
            local out_path = path.join(root, "src/ui/ui_assets_gen.hpp")
            local lines = {}
            lines[#lines + 1] = "// AUTO-GENERATED by xmake (fs_ui_wasm after_build) - do not edit."
            lines[#lines + 1] = "// Embedded web assets served by ui_server at runtime."
            lines[#lines + 1] = "#pragma once"
            lines[#lines + 1] = ""
            lines[#lines + 1] = "#include <cstddef>"
            lines[#lines + 1] = "#include <cstdint>"
            lines[#lines + 1] = "#include <string>"
            lines[#lines + 1] = "#include <vector>"
            lines[#lines + 1] = ""
            lines[#lines + 1] = "namespace fs {"
            lines[#lines + 1] = "namespace ui {"
            lines[#lines + 1] = "namespace assets {"
            lines[#lines + 1] = ""
            lines[#lines + 1] = "struct Asset {"
            lines[#lines + 1] = "  const char* name;"
            lines[#lines + 1] = "  const char* mime;"
            lines[#lines + 1] = "  const uint8_t* data;"
            lines[#lines + 1] = "  size_t size;"
            lines[#lines + 1] = "};"
            lines[#lines + 1] = ""
            local present = {}
            for _, it in ipairs(items) do
                local p = path.join(root, "web", it.name)
                -- NOTE: io.open(..,"rb") - xmake's io.readfile is text-mode and
                -- corrupts binary (UTF-8 replacement bytes), which breaks .wasm.
                local f = io.open(p, "rb")
                if not f then
                    raise("[assets] missing " .. p .. " (build fs_ui_wasm first)")
                end
                local data = f:read("*a")
                f:close()
                local ident = it.name:gsub("[^%w]", "_")
                lines[#lines + 1] = "static const uint8_t kData_" .. ident .. "[] = {"
                local buf = {}
                local n = #data
                for i = 1, n do
                    buf[#buf + 1] = tostring(string.byte(data, i))
                    if #buf == 24 then
                        lines[#lines + 1] = "  " .. table.concat(buf, ",") .. ","
                        buf = {}
                    end
                end
                if #buf > 0 then
                    lines[#lines + 1] = "  " .. table.concat(buf, ",") .. ","
                end
                lines[#lines + 1] = "};"
                lines[#lines + 1] = "static const size_t kSize_" .. ident .. " = " .. tostring(n) .. ";"
                lines[#lines + 1] = ""
                present[#present + 1] = {it = it, ident = ident}
            end
            lines[#lines + 1] = "static inline const std::vector<Asset>& kFiles() {"
            lines[#lines + 1] = "  static const std::vector<Asset> files = {"
            for _, e in ipairs(present) do
                lines[#lines + 1] = '      {"' .. e.it.name .. '", "' .. e.it.mime ..
                                   '", kData_' .. e.ident .. ", kSize_" .. e.ident .. "},"
            end
            lines[#lines + 1] = "  };"
            lines[#lines + 1] = "  return files;"
            lines[#lines + 1] = "}"
            lines[#lines + 1] = ""
            lines[#lines + 1] = "}  // namespace assets"
            lines[#lines + 1] = "}  // namespace ui"
            lines[#lines + 1] = "}  // namespace fs"
            lines[#lines + 1] = ""
            -- io.open wb (not io.writefile) for the same binary-safety reason
            local out = io.open(out_path, "wb")
            if not out then raise("[assets] cannot write " .. out_path) end
            out:write(table.concat(lines, "\n"))
            out:close()
            print("[assets] wrote " .. out_path .. " (" .. tostring(#present) .. " files)")
        end)
    end

target("flashsight")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files("src/main.cpp")
    add_deps("fs_core", "fs_ui")
    if whaleui_on then
        -- The desktop UI reads web/style.css through the VFS at run time, so the
        -- assets must exist next to the exe; they are tracked as inputs so a CSS
        -- edit re-links.
        add_headerfiles(web_assets)
    end
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "shell32", "ole32", "uuid", "crypt32", "bcrypt")
    end
    mbedtls_link()
    if whaleui_on then
        whaleui_copy_dlls()
    end

target("fs_tests")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files(dir_files("tests"))
    add_deps("fs_core", "fs_ui")
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "shell32", "ole32", "uuid", "crypt32", "bcrypt")
    end
    mbedtls_link()

end  -- not wasm
