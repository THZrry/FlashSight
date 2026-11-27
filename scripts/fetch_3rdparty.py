#!/usr/bin/env python3
"""Fetch FlashSight's 3rdparty dependencies into 3rdparty/.

One implementation for every host. The shell/batch versions this replaces
needed curl + unzip + tar, which a stock Windows box (and a fresh CI runner)
does not all have; Python 3 does, and it is already required by the icon
tooling in tools/.

    python scripts/fetch_3rdparty.py
    PROXY=http://127.0.0.1:10809 python scripts/fetch_3rdparty.py   # via proxy
    SKIP_WEBVIEW=1 python scripts/fetch_3rdparty.py                 # skip webview
    WHALEUI_DIR=C:/aaa/WhaleUI python scripts/fetch_3rdparty.py     # copy headers

3rdparty/ is NOT committed (see .gitignore); this script recreates it. It is
idempotent: existing files are overwritten, nothing is deleted.
"""
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TP = ROOT / "3rdparty"
TMP = ROOT / "temp"
PROXY = os.environ.get("PROXY", "").strip()
SKIP_WEBVIEW = bool(os.environ.get("SKIP_WEBVIEW", "").strip())
WHALEUI_DIR = os.environ.get("WHALEUI_DIR", "").strip()


def log(msg):
    print("[fetch] " + msg, flush=True)


def _opener():
    if not PROXY:
        return urllib.request.build_opener()
    log("using proxy " + PROXY)
    return urllib.request.build_opener(
        urllib.request.ProxyHandler({"http": PROXY, "https": PROXY}))


OPENER = _opener()


def download(url, dest):
    """Download url to dest. Skips the transfer if dest already has content."""
    dest = Path(dest)
    if dest.is_file() and dest.stat().st_size > 0:
        log("cached " + dest.name)
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    log("get " + url)
    tmp = dest.with_suffix(dest.suffix + ".part")
    with OPENER.open(url, timeout=180) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    tmp.replace(dest)
    return dest


def extract(archive, dest):
    """Extract a .zip / .tar.{gz,xz,bz2} archive into dest (created)."""
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    archive = str(archive)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            try:
                t.extractall(dest, filter="data")   # Python >= 3.12
            except TypeError:
                t.extractall(dest)


def fetch_archive(url, name, dest_dir, top=None):
    """Download an archive and unpack it under dest_dir.

    `top` is the directory the archive extracts to; when it already exists the
    download and the extraction are both skipped, which is what makes a second
    run fast. Returns dest_dir.
    """
    if top and (Path(dest_dir) / top).is_dir():
        log("cached " + top)
        return Path(dest_dir)
    archive = download(url, TMP / name)
    extract(archive, dest_dir)
    return Path(dest_dir)


def copy_tree_files(src_dir, dst_dir, names):
    """Copy the named files that exist in src_dir into dst_dir."""
    dst_dir = Path(dst_dir)
    dst_dir.mkdir(parents=True, exist_ok=True)
    copied = []
    for name in names:
        src = Path(src_dir) / name
        if src.is_file():
            shutil.copy2(src, dst_dir / Path(name).name)
            copied.append(name)
        else:
            log("note: %s not in %s" % (name, Path(src_dir).name))
    return copied


def main():
    TP.mkdir(exist_ok=True)
    TMP.mkdir(exist_ok=True)

    # --- gulrak/filesystem (ghc::filesystem: C++14 std::filesystem) ---
    fetch_archive(
        "https://github.com/gulrak/filesystem/archive/refs/tags/v1.5.14.tar.gz",
        "ghfs.tgz", TP, top="filesystem-1.5.14")

    # --- nlohmann/json (single header) ---
    download("https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp",
             TP / "json" / "json.hpp")
    # Compatibility include for external snippets/tools that spell
    # <nlohmann/json.hpp>. The project itself includes "json.hpp" (the
    # vendored layout); this shim makes both spellings work and is part of the
    # fetch script so a clean checkout gets it too.
    shim = TP / "json" / "nlohmann" / "json.hpp"
    shim.parent.mkdir(parents=True, exist_ok=True)
    shim.write_text('#pragma once\n#include "../json.hpp"\n', encoding="utf-8")

    # --- dtl (diff algorithm) ---
    fetch_archive(
        "https://github.com/cubicdaiya/dtl/archive/refs/tags/v1.20.zip",
        "dtl.zip", TP, top="dtl-1.20")

    # --- md4c (markdown) ---
    fetch_archive(
        "https://github.com/mity/md4c/archive/refs/heads/master.zip",
        "md4c.zip", TP, top="md4c-master")

    # --- cpp-httplib (single header) ---
    download("https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h",
             TP / "httplib" / "httplib.h")

    # --- mbedtls (TLS for the HTTPS provider clients) ---
    # Only the SOURCES are needed: xmake's `mbedtls_src` target compiles
    # library/*.c with the project's own toolchain. There is deliberately no
    # cmake build step - a prebuilt tree made with a different compiler (MSVC)
    # mixes two C runtimes and crashes on the first HTTPS request.
    fetch_archive(
        "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.4.zip",
        "mbedtls.zip", TP, top="mbedtls-3.6.4")
    fw = TP / "mbedtls-3.6.4" / "framework"
    if not fw.is_dir():
        # Not needed by the source build, but the mbedtls tree is unusable for
        # anything else (tests, the cmake build) without it.
        log("cloning mbedtls-framework")
        os.system('git clone -q --depth 1 '
                  'https://github.com/Mbed-TLS/mbedtls-framework.git "%s"' % fw)

    # --- dtl MinGW/MSVC patch: enableTrivial() const assigns a member ---
    diff_hpp = TP / "dtl-1.20" / "dtl" / "Diff.hpp"
    if diff_hpp.is_file():
        data = diff_hpp.read_bytes()
        old = b"bool               trivial;"
        new = b"mutable bool      trivial;"
        if old in data:
            diff_hpp.write_bytes(data.replace(old, new))
            log("patched " + str(diff_hpp.relative_to(ROOT)))

    # --- QuickJS regex engine (regex tool; same source WhaleJS uses) ---
    #
    # Pinned to the 2026-06-04 release: it is the one that exports
    # lre_get_alloc_count() (needed to size the capture array) and defines
    # LRE_FLAG_UNICODE. The 2024-01-13 tarball this script used to fetch has
    # neither, so regex_tools.cpp did not compile.
    #
    # cutils.c carries the DynBuf helpers libregexp/libunicode call into
    # (dbuf_put_*). quickjs-ng inlines those in cutils.h instead, which is why
    # the fallback copies fewer files - copy_tree_files skips what is absent and
    # xmake.lua only compiles cutils.c when it exists.
    qjs = TP / "qjs-regex"
    qjs.mkdir(parents=True, exist_ok=True)
    qjs_files = ["libregexp.c", "libunicode.c", "cutils.c", "cutils.h",
                 "libregexp.h", "libunicode.h", "libregexp-opcode.h",
                 "libunicode-table.h"]
    try:
        fetch_archive("https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz",
                      "qjs.tar.xz", TMP, top="quickjs-2026-06-04")
        copy_tree_files(TMP / "quickjs-2026-06-04", qjs, qjs_files)
    except Exception as e:  # noqa: BLE001 - bellard.org is often unreachable
        log("bellard.org failed (%s); falling back to quickjs-ng" % e)
        fetch_archive(
            "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.17.0.zip",
            "qjs-ng.zip", TMP, top="quickjs-0.17.0")
        copy_tree_files(TMP / "quickjs-0.17.0", qjs, qjs_files)
        # A leftover cutils.c from a previous bellard fetch would be stale
        # relative to the ng headers.
        if (qjs / "cutils.c").is_file():
            (qjs / "cutils.c").unlink()
            log("removed stale cutils.c (quickjs-ng inlines DynBuf)")

    # --- stb (image decode / resize / PNG encode; vision tools) ---
    # Three single-header public-domain files. Fetched individually because the
    # repo has no release tarball; stb_image_resize2 lives under deprecated/ in
    # the repo layout even though it is the current resize API.
    stb = TP / "stb"
    stb.mkdir(parents=True, exist_ok=True)
    for name, path in [
        ("stb_image.h", "stb_image.h"),
        ("stb_image_write.h", "stb_image_write.h"),
        ("stb_image_resize2.h", "deprecated/stb_image_resize2.h"),
    ]:
        shutil.copy2(download("https://raw.githubusercontent.com/nothings/stb/master/" + path,
                              TMP / name), stb / name)

    # --- reproc (subprocess management; rules/user.md) ---
    # Only sources + headers: xmake.lua picks the platform files per OS.
    reproc_zip = fetch_archive(
        "https://github.com/DaanDeMeyer/reproc/archive/refs/tags/v14.2.5.tar.gz",
        "reproc.tar.gz", TMP, top="reproc-14.2.5")
    reproc_src = reproc_zip / "reproc-14.2.5"
    inc = TP / "reproc" / "include" / "reproc"
    src = TP / "reproc" / "src"
    inc.mkdir(parents=True, exist_ok=True)
    src.mkdir(parents=True, exist_ok=True)
    for f in (reproc_src / "reproc" / "include" / "reproc").glob("*.h"):
        shutil.copy2(f, inc / f.name)
    for f in (reproc_src / "reproc" / "src").glob("*.c"):
        shutil.copy2(f, src / f.name)
    for f in (reproc_src / "reproc" / "src").glob("*.h"):
        shutil.copy2(f, src / f.name)
    shutil.copy2(reproc_src / "LICENSE", TP / "reproc" / "LICENSE")

    # --- webview (native window + OS web engine; MIT) ---
    # Header + one .cc. The MinGW compatibility header and the WebView2 SDK are
    # Windows-only; the GTK/Cocoa backends use the system headers.
    if not SKIP_WEBVIEW:
        wv_zip = fetch_archive(
            "https://github.com/webview/webview/archive/refs/tags/0.12.0.tar.gz",
            "webview.tar.gz", TMP, top="webview-0.12.0")
        wv = wv_zip / "webview-0.12.0"
        (TP / "webview" / "include" / "webview").mkdir(parents=True, exist_ok=True)
        (TP / "webview" / "src").mkdir(parents=True, exist_ok=True)
        shutil.copy2(wv / "core" / "include" / "webview.h",
                     TP / "webview" / "include" / "webview.h")
        shutil.copy2(wv / "core" / "include" / "webview" / "webview.h",
                     TP / "webview" / "include" / "webview" / "webview.h")
        shutil.copy2(wv / "core" / "src" / "webview.cc",
                     TP / "webview" / "src" / "webview.cc")
        shutil.copy2(wv / "LICENSE", TP / "webview" / "LICENSE")

        if os.name == "nt":
            # WebView2 SDK headers (the loader implementation is header-only).
            compat = TP / "webview" / "compat-mingw"
            compat.mkdir(parents=True, exist_ok=True)
            shutil.copy2(wv / "compatibility" / "mingw" / "include" / "EventToken.h",
                         compat / "EventToken.h")
            ms = download("https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2",
                          TMP / "mswebview2.zip")
            extract(ms, TMP / "mswebview2")
            sdk_inc = TP / "mswebview2" / "include"
            sdk_inc.mkdir(parents=True, exist_ok=True)
            src_inc = TMP / "mswebview2" / "build" / "native" / "include"
            for f in src_inc.glob("*.h"):
                shutil.copy2(f, sdk_inc / f.name)

    # --- WhaleUI headers (the WhaleUI variant; optional) ---
    if WHALEUI_DIR:
        inc_dir = Path(WHALEUI_DIR) / "include"
        if inc_dir.is_dir():
            dst = TP / "whaleui" / "include"
            dst.mkdir(parents=True, exist_ok=True)
            for name in ("whaleui.h", "whaleui.hpp"):
                if (inc_dir / name).is_file():
                    shutil.copy2(inc_dir / name, dst / name)
        else:
            log("warning: WHALEUI_DIR has no include/ dir: " + WHALEUI_DIR)

    log("done")


if __name__ == "__main__":
    sys.exit(main())
