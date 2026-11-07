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

-- CI stamps the release tag into the binary. Local builds keep the header's
-- default, so a dev build prints `FlashSight 0.1.0`.
local build_version = os.getenv("FLASHSIGHT_VERSION")
if build_version and build_version ~= "" then
    set_version(build_version)
    add_defines('FS_VERSION_STRING="' .. build_version .. '"')
end

add_rules("mode.debug", "mode.release")
set_languages("c++14")
-- Build targets sequentially in dependency order. The wasm bundle is a build
-- input to the native UI (its bytes are embedded in ui_assets_gen.hpp), and
-- xmake's default parallel-across-targets scheduling starts fs_ui's compile
-- before fs_ui_wasm has produced web/fs_ui.js. Serializing makes add_deps a
-- real ordering edge for compilation, not just linking.
--
-- "build.fence" is the modern spelling (xmake >= 3.0); the older
-- build.across_targets_in_parallel is deprecated and warns on every build.
if xmake.version() and xmake.version():ge("3.0.0") then
    set_policy("build.fence", true)
else
    set_policy("build.across_targets_in_parallel", false)
end

-- mbedtls is always built from source by the `mbedtls_src` target below, with
-- whatever toolchain the project uses.
--
-- The vendored cmake build under 3rdparty/mbedtls-3.6.4/build is NOT used, even
-- when it exists: on this machine it was produced by MSVC, so its archives
-- contain .obj files linked against the MSVC CRT. Linking those into a MinGW
-- executable mixes two heaps - allocations made by mbedtls/httplib went to one
-- CRT and were freed by the other, and every HTTPS request crashed with
-- SIGSEGV inside RtlFreeHeap when the httplib client was destroyed. Building
-- the same sources with the project toolchain makes allocation and release
-- symmetrical.
--
-- (xmake's mbedtls package was the other candidate; it installs via cmake and
-- therefore needs Visual Studio on Windows, which a MinGW-only box does not
-- have. Compiling the .c files directly has no such dependency.)

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
    "3rdparty/qjs-regex",
    "3rdparty/stb",
    "3rdparty/reproc/include",
    "3rdparty/whaleui/include",
    "3rdparty/webview/include",
    "3rdparty/mbedtls-3.6.4/include",
}
-- Windows-only vendored headers: the MinGW compatibility header and the
-- WebView2 SDK are not fetched on other hosts (see scripts/fetch_3rdparty.py),
-- so pointing -I at them would only add noise.
if is_plat("windows") then
    table.insert(includedirs, "3rdparty/webview/compat-mingw")
    table.insert(includedirs, "3rdparty/mswebview2/include")
end

-- THE variant selector. One option with three values, instead of two booleans:
--
--   xmake f --variant=web      (default) browser tab + embedded HTTP server
--   xmake f --variant=webview  native window via the OS web engine
--   xmake f --variant=whaleui  native window via WhaleUI's own renderer
--
-- Two booleans (--webview / --whaleui) could both be left set in the cached
-- config, and `xmake f --whaleui=y` never cleared --webview. That is how
-- "select WhaleUI" produced a Web build: the flags disagreed and the derived
-- variant silently fell back to web. A single valued option cannot disagree
-- with itself.
option("variant")
    set_default("web")
    set_values("web", "webview", "whaleui")
    set_showmenu(true)
    set_description("UI host: web (browser + local server) | webview | whaleui")
option_end()

-- path to a WhaleUI build (the directory holding libwhaleui-*.a); only used with
-- --variant=whaleui. Override with `xmake f --variant=whaleui --whaleui_libdir=...`
option("whaleui_libdir")
    set_default("C:/aaa/WhaleUI/build/mingw/x86_64/release")
    set_showmenu(true)
    set_description("Directory containing WhaleUI's built static library")
option_end()

-- WhaleUI checkout root. Only the SDL3 / SDL3_ttf import libs and DLLs are read
-- from it (WhaleUI ships them under 3rdparty/), so it must point at a clone of
-- the WhaleUI repo, not at the FlashSight tree. CI overrides this.
option("whaleui_dir")
    set_default("C:/aaa/WhaleUI")
    set_showmenu(true)
    set_description("WhaleUI checkout (supplies SDL3/SDL3_ttf libs and DLLs)")
option_end()

local variant = get_config("variant") or "web"
local whaleui_on = variant == "whaleui"
local webview_on = variant == "webview"

-- The three UI variants. They differ ONLY in the DOM backend under IDom:
--
--   web     (default) : WasmDom   - the UI compiled to wasm, run in a browser
--   webview           : WebViewDom - the UI in C++, DOM driven through webview
--   whaleui           : WhaleDom   - UI in C++, DOM driven by WhaleUI
--
-- The output name carries the variant (flashsight-web.exe / -webview.exe /
-- -whaleui.exe), so building one after another never silently replaces the
-- other, and no external script has to juggle output directories.
-- All variants share the same object directory: the variant macro is scoped to
-- the two or three files that branch on it, so switching variants recompiles
-- those and relinks, not the whole project.

-- Where WhaleUI's already-installed xrepo dependencies live (lexbor, utf8proc,
-- SDL3_image, SDL3). WhaleUI built them from source; we link the same copies
-- instead of declaring add_requires(), because re-installing lexbor needs a VS
-- toolchain this machine does not have. Override if your xmake package cache is
-- elsewhere:  xmake f --variant=whaleui --whaleui_pkgdir=D:/xmake/packages
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
elseif is_plat("macosx", "iphoneos") then
    -- set_languages("c++14") passes -std=c++14, which makes clang define
    -- __STRICT_ANSI__ and hides part of the BSD extension surface (nanosleep,
    -- mkstemp). _DARWIN_C_SOURCE restores the default Darwin feature level.
    add_defines("_DARWIN_C_SOURCE")
else
    -- Same for glibc: -std=c++14 defines __STRICT_ANSI__, so popen/pclose,
    -- mkstemp and nanosleep (used by fs_git, shell_detect and main) are not
    -- declared without an explicit feature macro.
    add_defines("_DEFAULT_SOURCE")
end

-- Which desktop host is compiled in.
--
-- The variant used to be a PROJECT-WIDE add_defines(). A global define is part
-- of every translation unit's command line, so switching variants invalidated
-- every object and `xmake f --variant=whaleui && xmake b` recompiled the whole
-- world - the user's complaint was exactly this ("其实把 main 提出来只需要重编
-- 译一个文件才对").
--
-- Now the macro is defined only on the targets that contain branching files:
--
--   fs_ui       : shared UI code, NO variant macro
--   fs_ui_host  : just the two backend files of the selected variant
--   flashsight  : main.cpp (the only other file that branches)
--
-- Switching variants therefore recompiles three files and relinks.
local host_files = {}
if whaleui_on then
    host_files = {"src/ui/ui_dom_whale.cpp", "src/ui/ui_main_whale.cpp"}
    add_linkdirs(get_config("whaleui_libdir"))
elseif webview_on then
    host_files = {"src/ui/ui_dom_webview.cpp", "src/ui/ui_main_webview.cpp"}
end

-- Generated headers live in the BUILD tree (not in src/), at ONE stable path.
--
-- They used to be written into src/ui/ under a single name shared by all
-- variants: building web and then webview had one variant's pack overwrite the
-- other's. Keeping generated files out of the source tree fixes that.
--
-- The directory must be STABLE across variants, not per variant: the include
-- path is part of every object's command line, so a per-variant directory made
-- xmake recompile the whole of fs_ui on every switch. With one path only the
-- files that actually include the header (ui_server.cpp, ui_main_webview.cpp)
-- are rebuilt.
local gen_dir = path.join(os.projectdir(), "build", "gen")
-- Files that belong to exactly one variant and must NOT be compiled by fs_ui:
-- each is guarded by its own macro, but compiling them unconditionally made
-- their (empty) objects part of fs_ui, which meant the variant macro had to be
-- project-wide to make them non-empty. They now live in fs_ui_host / the wasm
-- target only.
local variant_only_files = {
    "src/ui/ui_dom_whale.cpp",   "src/ui/ui_main_whale.cpp",
    "src/ui/ui_dom_webview.cpp", "src/ui/ui_main_webview.cpp",
    "src/ui/ui_dom_wasm.cpp",    "src/ui/ui_main_wasm.cpp",
    "src/ui/ui_http_backend.cpp",
}
local function is_variant_only(f)
    for _, v in ipairs(variant_only_files) do
        if path.filename(f) == path.filename(v) then return true end
    end
    return false
end

-- Shared UI sources: every .cpp under src/ui except the per-variant ones.
local function shared_ui_files()
    local out = {}
    for _, f in ipairs(dir_files("src/ui")) do
        if not is_variant_only(f) then table.insert(out, f) end
    end
    return out
end

-- httplib's TLS backend must be selected in EVERY translation unit that
-- includes the header, not just fs_core's. ui_server.cpp (in fs_ui) also
-- includes httplib.h for the local HTTP server; compiled without this macro it
-- produced a second, non-TLS copy of the header's inline definitions, and the
-- linker picked those - so SSLClient silently behaved like a plain client and
-- every HTTPS request failed with "Could not establish connection" while a
-- standalone request in the same process succeeded.
add_defines("CPPHTTPLIB_MBEDTLS_SUPPORT")

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
    -- Syntax highlighting. Data-driven (one scanner + per-language word lists)
    -- rather than a generated DFA per language: see fs_highlight.hpp for why.
    "src/common/fs_highlight.cpp",
    -- Model-name coarse matching (thinking levels + brand icon). It has no
    -- dependencies beyond <string>/<vector> precisely so BOTH builds can compile
    -- it: the backend fills these in on model.list, and the wasm UI needs them to
    -- preview a model's levels while its name is still being typed.
    "src/provider/model_naming.cpp",
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
    "web/icons/icons.css",
}

-- Icon sets are directories of many small SVGs. They are inputs to the packer,
-- so they need the same dependency edge as web_assets: enumerating them here
-- (rather than at pack time) is what makes editing an SVG rebuild the exe.
for _, d in ipairs({"web/icons/lobe", "web/icons/file"}) do
    for _, f in ipairs(os.files(d .. "/*.svg") or {}) do
        table.insert(web_assets, f)
    end
    for _, f in ipairs(os.files(d .. "/manifest.json") or {}) do
        table.insert(web_assets, f)
    end
end

-- ==========================================================================
-- wasm UI target: built as a dependency of fs_ui (per-target platform).
-- ==========================================================================
-- emcc on PATH? (search PATH entries for emcc)
--
-- Two things are host-specific here:
--   * the separator: ';' on Windows, ':' elsewhere. Splitting on ';'
--     unconditionally made this always false on Linux/macOS, which silently
--     skipped the wasm target and produced a Web build with no fs_ui.js.
--   * the filename: an emsdk install on Windows ships emcc.bat / emcc.py, not
--     emcc.exe, so the extensionless name must be resolved like a shell would.
local function has_emcc()
    local env = os.getenv("PATH") or ""
    local sep = is_host("windows") and ";" or ":"
    local names = is_host("windows")
        and {"emcc", "emcc.exe", "emcc.bat", "emcc.cmd", "emcc.py"}
        or {"emcc"}
    for entry in env:gmatch("[^" .. sep .. "]+") do
        for _, name in ipairs(names) do
            if os.isfile(entry .. "/" .. name) then
                return true
            end
        end
    end
    return false
end
local emcc_ok = has_emcc()

-- The Web variant IS the wasm bundle: without emcc the build would succeed but
-- embed no fs_ui.js/fs_ui.wasm, and the browser would show a stale or blank
-- app (the asset packer only lists files that exist). Fail here instead.
if variant == "web" and not emcc_ok then
    raise("emcc not found on PATH - the Web variant needs Emscripten to build " ..
          "the wasm UI.\n" ..
          "  * install emsdk and run `emsdk activate latest`, or\n" ..
          "  * build a desktop variant instead: xmake f --variant=webview")
end

-- Only the Web variant has any use for the wasm bundle. Building it for the
-- desktop variants cost a full emcc run on every build for output nobody
-- embedded.
if variant == "web" and emcc_ok then
target("fs_ui_wasm")
    set_kind("binary")
    set_plat("wasm")
    -- The arch must be overridden too: --arch=arm64 (or any other host arch)
    -- would otherwise be carried into the wasm target, which only knows
    -- wasm32/wasm64 - so every arm64 CI job would fail to configure it. The
    -- wasm bundle is architecture-independent anyway.
    set_arch("wasm32")
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
    add_ldflags("-sEXPORTED_FUNCTIONS=_fs_ui_start,_fs_ui_show,_fs_ui_fire_event,_fs_ui_fire,_fs_ui_drop,_fs_ui_split,_fs_ui_vsplit,_fs_ui_key,_fs_ui_key_input,_fs_ui_cell_size,_fs_ui_file_pick,_fs_ui_scroll_pin,_fs_ui_scroll_edges,_fs_ui_timeout,_fs_api_done,_fs_api_sse_event,_fs_ui_backend_debug,_fs_ui_debug,_malloc,_free",
                {force = true})
    add_ldflags("-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8",
                {force = true})
    -- Nothing to pack here: the packing runs in fs_ui's before_build (see there
    -- for why after_build here was too late to invalidate fs_ui's sources).
end  -- fs_ui_wasm

-- ============================ native ============================
if not is_plat("wasm") then

-- mbedtls (fs_net_http.cpp references its symbols).
--
-- Always the source build below - see the note at the top of the file for why
-- the vendored MSVC archives must not be linked into a MinGW executable.
function mbedtls_link()
    if is_plat("windows") then
        add_syslinks("bcrypt", "crypt32")
    end
end

-- mbedtls from source, compiled by the project's own toolchain.
--
-- Only library/*.c: with the default mbedtls_config.h neither the everest nor
-- the p256-m driver is enabled, so their sources are not needed (cmake builds
-- them unconditionally but only links them in when the config asks).
--
-- Link order matters as usual: callers must depend on this AFTER fs_core, or
-- every mbedtls_* symbol fs_core references stays undefined.
target("mbedtls_src")
    set_kind("static")
    set_languages("c99")
    add_includedirs("3rdparty/mbedtls-3.6.4/include",
                    "3rdparty/mbedtls-3.6.4/library")
    add_files("3rdparty/mbedtls-3.6.4/library/*.c")
    -- mbedtls is third-party C: warnings here are not ours to fix.
    set_warnings("none")
target_end()

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
    local whaleui_dir = get_config("whaleui_dir")
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
    -- SDL3_image is a STATIC archive and calls into SDL3, so it must appear
    -- BEFORE -lSDL3 on the command line. It cannot go through add_ldflags():
    -- xmake emits absolute .a paths after every -l entry, which put it last and
    -- the link failed with "undefined reference to SDL_SeekIO" (IMG.c.obj).
    -- Registering it as a link with its own -L keeps the order explicit.
    local sdl3_image = find_pkg_lib("libsdl3_image", "libSDL3_image.a")
    if sdl3_image then
        add_linkdirs(path.directory(sdl3_image))
        add_links("SDL3_image")
    else
        print("[whaleui] warning: libsdl3_image not found under " .. pkg)
    end
    -- SDL3 / SDL3_ttf: prebuilt import libs (they load their DLLs at run time)
    if os.isdir(path.join(whaleui_dir, "3rdparty/sdl3/lib")) then
        add_linkdirs(path.join(whaleui_dir, "3rdparty/sdl3/lib"))
        add_links("SDL3")
    end
    if os.isdir(path.join(whaleui_dir, "3rdparty/sdl3_ttf/lib")) then
        add_linkdirs(path.join(whaleui_dir, "3rdparty/sdl3_ttf/lib"))
        add_links("SDL3_ttf")
    end
    -- remaining static xrepo dependencies (no SDL dependency, order irrelevant)
    local statics = {
        {name = "lexbor", lib = "liblexbor_static.a"},
        {name = "utf8proc", lib = "libutf8proc.a"},
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
    local whaleui_dir = get_config("whaleui_dir")
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
        -- the UI's stylesheet + icon assets, in the layout the app asks for
        local webdir = path.join(outdir, "web")
        os.mkdir(webdir)
        os.mkdir(path.join(webdir, "icons"))
        for _, rel in ipairs({"style.css", "index.html", "icons/fa-subset.css",
                              "icons/icons.css", "icons/fa-solid-900.woff2"}) do
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
    add_files(dir_files("src/core", "src/agent", "src/tools",
                        "src/provider", "src/storage", "src/common", "src/term"))
    add_files("3rdparty/md4c-master/src/md4c.c",
              "3rdparty/md4c-master/src/md4c-html.c",
              "3rdparty/md4c-master/src/entity.c")
    -- QuickJS regex engine. cutils.c carries the DynBuf helpers that
    -- libregexp/libunicode call into (dbuf_put_*, dbuf_free); quickjs-ng
    -- inlines them in cutils.h and ships no .c, so the file is optional.
    local qjs_files = {"3rdparty/qjs-regex/libregexp.c",
                       "3rdparty/qjs-regex/libunicode.c"}
    if os.isfile("3rdparty/qjs-regex/cutils.c") then
        table.insert(qjs_files, "3rdparty/qjs-regex/cutils.c")
    end
    add_files(qjs_files)
    -- fs_net_http.cpp is an httplib TLS client, so fs_core needs mbedtls. Declare
    -- it here, not only on the exe: link order follows the dependency graph, and
    -- with mbedtls listed only as a sibling of fs_core the archive landed BEFORE
    -- it and every mbedtls_* reference stayed undefined.
    add_deps("mbedtls_src")
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
        -- gdi32: screen capture fallback (fs_screen.cpp BitBlt/GetDIBits).
        -- d3d11/dxgi: Desktop Duplication, the primary capture path (it reads the
        -- DWM-composited surface, which GDI cannot on several real setups).
        add_syslinks("ws2_32", "user32", "gdi32", "d3d11", "dxgi", "shell32",
                     "ole32", "uuid", "crypt32", "bcrypt", "psapi")
    else
        -- std::thread (the UI server's listen thread, the terminal pollers,
        -- reproc's drain threads) needs -pthread; dl is what the webview Linux
        -- backend expects to be present.
        add_syslinks("pthread", "dl")
        if is_plat("linux", "bsd") then
            -- fs_screen.cpp captures through X11. The X11 headers are a normal
            -- distro package (libx11-dev); without them the capture backend
            -- compiles to a "no display" stub only if we guard it, so require
            -- the library here where the platform can actually provide it.
            add_syslinks("X11")
        end
    end


target("fs_ui")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    -- The packed asset header is generated per variant into the build tree (see
    -- the packer below). It must EXIST before any compile: xmake resolves
    -- includes during dependency scanning, which happens before before_build
    -- writes the real pack. on_load runs at load time, where io/os are in scope,
    -- and drops an empty stub if there is nothing there yet.
    add_includedirs(gen_dir)
    on_load(function (target)
        if not os.isdir(gen_dir) then os.mkdir(gen_dir) end
        local p = path.join(gen_dir, "ui_assets_gen.hpp")
        if not os.isfile(p) then
            io.writefile(p,
                "// placeholder - xmake rewrites this during the build\n" ..
                "#pragma once\n" ..
                "#include <cstddef>\n#include <cstdint>\n#include <string>\n" ..
                "#include <vector>\n" ..
                "namespace fs { namespace ui { namespace assets {\n" ..
                "struct Asset { const char* name; const char* mime;" ..
                " const uint8_t* data; size_t size; };\n" ..
                "static inline const std::vector<Asset>& kFiles() {\n" ..
                "  static const std::vector<Asset> files;\n" ..
                "  return files;\n" ..
                "}\n" ..
                "} } }\n")
        end
    end)
    -- Shared UI only: the per-variant backends live in fs_ui_host, so this
    -- library has NO variant macro and its objects stay valid across a variant
    -- switch (this is what makes switching cheap).
    add_files(shared_ui_files())
    -- fs_ui calls into fs_core (highlight, settings_store, fs_vt). Declaring it
    -- HERE (rather than only on the exe) is what puts fs_core AFTER fs_ui on the
    -- link line: with fs_core first, every fs::highlight reference from
    -- ui_app.cpp was already past its archive and the link failed. Declaring
    -- three sibling deps on the exe let xmake order the independent ones
    -- alphabetically, which reintroduced exactly that.
    add_deps("fs_core")
    -- The packed assets are INPUTS of this target: the packer runs in its
    -- before_build and writes ui_assets_gen.hpp, so editing index.html or a CSS
    -- file has to invalidate fs_ui or the new bytes never reach the library
    -- (which is how a change to index.html silently did nothing).
    add_headerfiles(web_assets)
    -- ui_server.cpp stays in EVERY variant: all three embed the local HTTP
    -- server (it is the groundwork for remote access, see the note at the top).
    if whaleui_on then
        whaleui_link()
    end
    add_cxxflags("-Wall")
    if emcc_ok and not whaleui_on and not webview_on then
        -- fs_ui_wasm produces web/fs_ui.js + fs_ui.wasm, which the packer embeds.
        -- Only the Web variant ships the wasm bundle; the desktop variants host
        -- the UI natively and have no use for it.
        add_deps("fs_ui_wasm", {link = false})
    end
    -- Pack the assets BEFORE fs_ui's own sources are checked for staleness:
    -- a header regenerated inside a dependency's after_build is already "not
    -- newer" by then, so a style.css edit never reached this target.
    --
    -- Every variant embeds the stylesheet + icons (the Web variant serves them
    -- over HTTP, the webview variant inlines them into the document, the
    -- WhaleUI variant reads them through its VFS). Only the Web variant also
    -- embeds the compiled wasm bundle.
    --
    -- NOTE: the body is INLINED here on purpose. xmake only exposes io/os/path
    -- inside a target hook's own body; a script-level function cannot see them
    -- ("attempt to index a nil value (local 'io_lib')" / "call a nil value").
    if true then
        local with_wasm = (variant == "web") and emcc_ok
        before_build(function (target)
            local root = os.projectdir()
            os.mkdir(gen_dir)
            local items = {
                {name = "index.html", mime = "text/html; charset=utf-8"},
                {name = "style.css",  mime = "text/css; charset=utf-8"},
                {name = "icons/fa-subset.css", mime = "text/css; charset=utf-8"},
                {name = "icons/fa-solid-900.woff2", mime = "font/woff2"},
                {name = "icons/icons.css", mime = "text/css; charset=utf-8"},
            }
            if with_wasm then
                items[#items + 1] = {name = "fs_ui.js",   mime = "application/javascript; charset=utf-8"}
                items[#items + 1] = {name = "fs_ui.wasm", mime = "application/wasm"}
            end
            for _, d in ipairs({"icons/lobe", "icons/file"}) do
                local dir = path.join(root, "web", d)
                for _, f in ipairs(os.files(path.join(dir, "*.svg")) or {}) do
                    items[#items + 1] = {name = d .. "/" .. path.filename(f),
                                         mime = "image/svg+xml"}
                end
                for _, f in ipairs(os.files(path.join(dir, "*.json")) or {}) do
                    items[#items + 1] = {name = d .. "/" .. path.filename(f),
                                         mime = "application/json; charset=utf-8"}
                end
            end
            local out_path = path.join(gen_dir, "ui_assets_gen.hpp")
            local lines = {}
            lines[#lines + 1] = "// AUTO-GENERATED by xmake (pack_web_assets) - do not edit."
            lines[#lines + 1] = "// Embedded web assets: served over HTTP (Web build) or inlined"
            lines[#lines + 1] = "// into the webview document (Desktop build)."
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
                    raise("[assets] missing " .. p)
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
            -- Write only when the bytes actually differ. The hook runs before
            -- every build, and unconditionally rewriting the header gave it a
            -- fresh mtime each time, which recompiled ui_server.cpp (its only
            -- includer) on every `xmake b` even when nothing had changed.
            local text = table.concat(lines, "\n")
            local same = false
            local rd = io.open(out_path, "rb")
            if rd then
                same = (rd:read("*a") == text)
                rd:close()
            end
            if same then
                print("[assets] unchanged (" .. tostring(#present) .. " files)")
            else
                -- io.open wb (not io.writefile) for the same binary-safety reason
                local out = io.open(out_path, "wb")
                if not out then raise("[assets] cannot write " .. out_path) end
                out:write(text)
                out:close()
                print("[assets] wrote " .. out_path .. " (" .. tostring(#present) .. " files)")
            end
        end)
    end


-- ==========================================================================
-- The three UI variants (see the `variant` note at the top)
--
--   web     : WasmDom     - UI compiled to wasm, run in a browser tab
--   webview : WebViewDom  - native window, DOM driven through webview
--   whaleui : WhaleDom    - native window, DOM driven by WhaleUI
--
-- Only the DOM backend and the window differ. The variant macro is applied to
-- fs_ui_host and flashsight ONLY, so `xmake f --variant=... && xmake b`
-- recompiles two backend files plus main.cpp.
-- ==========================================================================

-- Per-variant backend files, compiled with that variant's macro.
if #host_files > 0 then
target("fs_ui_host")
    set_kind("static")
    set_languages("c++14")
    add_includedirs(includedirs)
    -- ui_main_webview.cpp includes the generated asset header.
    add_includedirs(gen_dir)
    add_files(host_files)
    add_defines(whaleui_on and "FS_UI_WHALEUI" or "FS_UI_WEBVIEW")
    add_deps("fs_ui", "fs_core")
    add_deps("mbedtls_src")
    add_cxxflags("-Wall")
    if whaleui_on then
        whaleui_link()
    elseif webview_on then
        add_deps("webview_host")
        if is_plat("windows") then
            add_syslinks("advapi32", "version")
        end
    end
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "gdi32", "d3d11", "dxgi", "shell32",
                     "ole32", "uuid", "crypt32", "bcrypt", "psapi")
    else
        add_syslinks("pthread", "dl")
    end
    mbedtls_link()
end

target("flashsight")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_includedirs(gen_dir)
    add_files("src/main.cpp")
    -- The output carries the variant, so building two variants in the same
    -- output directory never silently replaces one with the other (that is how
    -- "I selected WhaleUI and got a Web package" happened).
    set_basename("flashsight-" .. variant)
    -- fs_ui BEFORE fs_core: ui_app.cpp calls into fs_core (fs::highlight,
    -- settings_store, fs_vt), so the linker must see fs_ui's undefined
    -- references while fs_core is still ahead of it on the command line.
    -- With the old order (-lfs_core -lfs_ui) it failed with
    -- "undefined reference to fs::highlight::lang_for_path" - the browser
    -- build never noticed because nothing there pulls ui_app.cpp.obj in.
    add_deps("fs_ui", "fs_core")
    add_deps("mbedtls_src")
    -- main.cpp is the only other file that branches on the variant.
    if whaleui_on then
        add_deps("fs_ui_host")
        add_defines("FS_UI_WHALEUI")
        -- The desktop UI reads web/style.css through the VFS at run time, so the
        -- assets must exist next to the exe; they are tracked as inputs so a CSS
        -- edit re-links.
        add_headerfiles(web_assets)
    elseif webview_on then
        add_deps("fs_ui_host")
        add_defines("FS_UI_WEBVIEW")
        if is_plat("windows") then
            add_syslinks("advapi32", "version")
        end
    end
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "gdi32", "d3d11", "dxgi", "shell32",
                     "ole32", "uuid", "crypt32", "bcrypt", "psapi")
    else
        -- see fs_core: -pthread for std::thread, dl for the webview backend
        add_syslinks("pthread", "dl")
    end
    mbedtls_link()
    if whaleui_on then
        whaleui_copy_dlls()
    end

-- ==========================================================================
-- webview host library (only with --variant=webview).
--
-- `webview` (github.com/webview/webview, MIT) is a header + one .cc that opens
-- a native window with the OS web engine:
--
--   Windows : WebView2 (needs the SDK headers in 3rdparty/mswebview2; the
--             runtime ships with Windows 11 and current Windows 10)
--   macOS   : WKWebView (header-only against the system frameworks, so just
--             -framework WebKit)
--   Linux   : WebKitGTK via GTK3/GTK4 - the headers/flags come from pkg-config,
--             so the runner needs libgtk-3-dev + libwebkit2gtk-4.1-dev
--
-- See doc-internal/thirdparty.md.
-- ==========================================================================
if webview_on and not is_plat("wasm") then
    target("webview_host")
        set_kind("static")
        set_languages("c++14")
        add_includedirs("3rdparty/webview/include")
        add_files("3rdparty/webview/src/webview.cc")
        add_defines("WEBVIEW_STATIC")
        add_cxxflags("-Wall")
        if is_plat("windows") then
            -- The built-in WebView2 loader reads the SDK headers and resolves
            -- WebView2Loader.dll's exports at run time, so no import lib is
            -- linked. shlwapi: the loader uses PathFindFileNameW.
            add_includedirs("3rdparty/webview/compat-mingw",
                            "3rdparty/mswebview2/include")
            add_syslinks("ole32", "uuid", "advapi32", "shell32", "user32", "shlwapi")
        elseif is_plat("macosx", "iphoneos") then
            -- webview.cc is plain C++ that talks to the ObjC runtime through
            -- objc_msgSend (no @-syntax, no .mm), so no ObjC++ is involved.
            -- This mirrors webview's own cmake: WebKit (which brings AppKit
            -- and CoreGraphics with it) plus libdl.
            add_frameworks("WebKit")
            add_syslinks("dl")
        elseif is_plat("linux", "bsd") then
            -- GTK3 + WebKitGTK 4.1 is the oldest pairing webview supports that
            -- current distros still package. webview.h picks the GTK3/GTK4 code
            -- path from GTK_MAJOR_VERSION, so both flag sets must come from the
            -- same pair. find_package returns structured links/linkdirs/
            -- includedirs; splitting pkg-config's output by hand would lose the
            -- ordering xmake applies to links (and break on paths with spaces).
            local gtk = find_package("pkgconfig::gtk+-3.0")
            local wk = find_package("pkgconfig::webkit2gtk-4.1")
            if not gtk or not wk then
                raise("--variant=webview on Linux needs libgtk-3-dev and libwebkit2gtk-4.1-dev " ..
                      "(pkg-config must find gtk+-3.0 and webkit2gtk-4.1)")
            end
            for _, pkg in ipairs({gtk, wk}) do
                add_includedirs(pkg.includedirs or {})
                add_defines(pkg.defines or {})
                add_linkdirs(pkg.linkdirs or {})
                add_links(pkg.links or {})
            end
            add_syslinks("dl")
        end
    target_end()
end

target("fs_tests")
    set_kind("binary")
    set_languages("c++14")
    add_includedirs(includedirs)
    add_files(dir_files("tests"))
    -- same link order as `flashsight`: the tests link UiApp, which calls into
    -- fs_core (see the note there)
    add_deps("fs_ui", "fs_core")
    add_deps("mbedtls_src")
    add_cxxflags("-Wall")
    if is_plat("windows") then
        add_syslinks("ws2_32", "user32", "gdi32", "d3d11", "dxgi", "shell32",
                     "ole32", "uuid", "crypt32", "bcrypt", "psapi")
    else
        -- std::thread (the UI server's listen thread, the terminal pollers) needs
        -- -pthread; dlopen is used by nothing yet but the webview Linux backend
        -- expects libdl to be present.
        add_syslinks("pthread", "dl")
        if is_plat("linux", "bsd") then
            add_syslinks("X11")  -- fs_screen.cpp (tests exercise the capture path)
        end
    end
    mbedtls_link()

end  -- not wasm
