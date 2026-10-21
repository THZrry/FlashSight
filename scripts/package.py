#!/usr/bin/env python3
"""Package a built FlashSight variant into a release archive.

    python scripts/package.py --variant web     --build-dir build/web     --out dist
    python scripts/package.py --variant webview --build-dir build/webview --out dist
    python scripts/package.py --variant webview --build-dir build/webview --out dist \
        --version v0.1.0 --platform windows-x86_64

What goes in the archive
------------------------
The Web variant is a single executable: the wasm bundle, stylesheet and icons
are all embedded in it (ui_assets_gen.hpp) and served by the embedded HTTP
server, so there is nothing else to ship.

The Webview variant is also self-contained as far as the UI goes (the
document is built from the embedded assets), but the executable itself needs
the C++ runtime it was linked against:

  * Windows/MinGW: libstdc++-6.dll + libgcc_s_seh-1.dll (+ winpthread on some
    toolchains). The mingw toolchain links them dynamically by default, so
    they must sit next to the exe or it fails to start on a machine without
    MinGW installed.
  * macOS: the system runtime, nothing to add.
  * Linux: glibc/GTK from the distro, nothing to add (the archive documents
    the WebKitGTK package in README.txt instead of vendoring it).

Archive layout
--------------
    flashsight-<version>-<platform>-<variant>/
        flashsight[.exe]
        README.txt
        *.dll                (Windows only)
"""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# DLLs that every Windows machine has. Anything else the executable imports is
# a toolchain runtime that must travel with it: MinGW/GCC needs libstdc++-6.dll
# and libgcc_s_*.dll, llvm-mingw needs libc++.dll and libunwind.dll, and which
# of those applies depends on the toolchain, so the list is read from the
# import table rather than hardcoded.
WINDOWS_SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll",
    "dwmapi.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll", "kernel32.dll",
    "msvcrt.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll", "psapi.dll",
    "rpcrt4.dll", "secur32.dll", "setupapi.dll", "shell32.dll", "shlwapi.dll",
    "user32.dll", "userenv.dll", "uuid.dll", "uxtheme.dll", "version.dll",
    "winhttp.dll", "wininet.dll", "winmm.dll", "winspool.drv", "ws2_32.dll",
    "wtsapi32.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "api-ms-win-crt",
}


def log(msg):
    print("[package] " + msg, flush=True)


def find_exe(build_dir):
    """Locate the built executable under build_dir (xmake nests by plat/arch/mode).

    xmake names the target after the target name, so both variants produce
    `flashsight[.exe]`. A suffixed name (`flashsight_web.exe`) would mean the
    target was renamed - accept it too rather than failing the release on a
    naming change nobody remembers making.
    """
    names = ("flashsight.exe", "flashsight")
    hits = []
    for name in names:
        for p in build_dir.rglob(name):
            # Skip xmake's internal trees: .deps/ holds dependency stubs with
            # the same basename, .objs/ holds object files.
            if any(part.startswith(".") for part in p.relative_to(build_dir).parts):
                continue
            if not p.is_file():
                continue
            hits.append(p)
    if not hits:
        # Fallback: any executable whose name starts with "flashsight".
        for p in build_dir.rglob("flashsight*"):
            if p.suffix.lower() not in (".exe", ""):
                continue
            if any(part.startswith(".") for part in p.relative_to(build_dir).parts):
                continue
            if p.is_file():
                hits.append(p)
    if not hits:
        raise SystemExit("no flashsight executable under %s" % build_dir)
    # Prefer the shallowest path, and a release build over a stale debug tree.
    hits.sort(key=lambda p: (len(p.parts), "debug" in p.parts))
    return hits[0]


def mingw_bin_dirs():
    """Directories that may hold the toolchain runtime DLLs."""
    dirs = []
    for tool in ("g++", "x86_64-w64-mingw32-g++", "aarch64-w64-mingw32-g++"):
        exe = shutil.which(tool)
        if exe:
            dirs.append(Path(exe).resolve().parent)
    for env in ("MINGW_PREFIX", "MSYSTEM_PREFIX", "LLVM_MINGW_DIR", "LLVM_MINGW_ROOT"):
        val = os.environ.get(env)
        if val:
            p = Path(val)
            dirs.append(p / "bin")
            dirs.append(p)
    return dirs


def imported_dlls(exe):
    """Names of the DLLs an executable imports (via objdump -p)."""
    if not shutil.which("objdump"):
        log("warning: objdump not found; cannot inspect the import table")
        return set()
    try:
        dump = subprocess.run(["objdump", "-p", str(exe)], capture_output=True,
                              text=True, check=False).stdout
    except OSError:
        return set()
    names = set()
    for line in dump.splitlines():
        line = line.strip()
        if line.startswith("DLL Name:"):
            names.add(line.split(":", 1)[1].strip())
    return names


def write_readme(dest, variant, version, platform):
    lines = [
        "FlashSight %s" % version,
        "UI variant: %s" % variant,
        "Platform: %s" % platform,
        "",
        "Run:",
        "  flashsight%s" % (".exe" if platform.startswith("windows") else ""),
        "",
        "Options:",
        "  --port N       serve the UI on a fixed port (default: a free one)",
        "  --no-browser   do not open the system browser",
        "  --root PATH    data directory (default: ~/.flashsight)",
        "",
        "Both variants embed the local HTTP server; the Web variant shows the UI",
        "in your browser, the Webview variant opens a native window and serves the",
        "same API over HTTP.",
        "",
    ]
    if variant == "webview" and platform.startswith("linux"):
        lines += [
            "Runtime requirement: WebKitGTK 4.1 + GTK 3, e.g. on Debian/Ubuntu:",
            "  sudo apt install libgtk-3-0 libwebkit2gtk-4.1-0",
            "",
        ]
    if variant == "webview" and platform.startswith("windows"):
        lines += [
            "Runtime requirement: WebView2 Runtime. It ships with Windows 11 and",
            "current Windows 10; otherwise install the Evergreen Runtime from",
            "https://developer.microsoft.com/microsoft-edge/webview2/",
            "",
        ]
    if variant == "web":
        lines += [
            "The UI is compiled to WebAssembly and embedded in the executable;",
            "no separate asset files are needed.",
            "",
        ]
    dest.write_text("\n".join(lines), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", required=True, choices=["web", "webview", "whaleui"])
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--out", default="dist")
    ap.add_argument("--version", default="0.1.0")
    ap.add_argument("--platform", default=None,
                    help="e.g. windows-x86_64 (default: derived from the host)")
    args = ap.parse_args()

    build_dir = (ROOT / args.build_dir).resolve()
    exe = find_exe(build_dir)
    log("exe: %s" % exe)

    platform = args.platform or sys.platform
    version = args.version
    name = "flashsight-%s-%s-%s" % (version, platform, args.variant)

    out_dir = (ROOT / args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    stage = out_dir / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    shutil.copy2(exe, stage / exe.name)

    # Windows: bundle the C++ runtime the exe was linked against. The import
    # table decides which DLLs that is - a statically linked toolchain must not
    # get a stray DLL next to the exe, and llvm-mingw's libc++.dll must not be
    # missed just because it is not in a hardcoded MinGW list.
    if exe.suffix.lower() == ".exe":
        imported = imported_dlls(exe)
        search = [exe.parent] + mingw_bin_dirs()
        for dll in sorted(imported):
            if dll.lower() in WINDOWS_SYSTEM_DLLS:
                continue
            if dll.lower().startswith("api-ms-win-"):
                continue
            for d in search:
                src = d / dll
                if src.is_file():
                    shutil.copy2(src, stage / dll)
                    log("runtime: " + dll)
                    break
            else:
                log("warning: %s imported but not found next to the toolchain"
                    % dll)

    write_readme(stage / "README.txt", args.variant, version, platform)

    # Archive: .zip on Windows (what users expect), .tar.gz elsewhere (keeps
    # the executable bit, which a zip loses).
    if platform.startswith("windows"):
        archive = out_dir / (name + ".zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for f in sorted(stage.rglob("*")):
                if f.is_file():
                    z.write(f, f.relative_to(out_dir))
    else:
        archive = out_dir / (name + ".tar.gz")
        with tarfile.open(archive, "w:gz") as t:
            t.add(stage, arcname=name)
    log("wrote %s (%.1f MB)" % (archive, archive.stat().st_size / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
