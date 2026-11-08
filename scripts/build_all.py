#!/usr/bin/env python3
"""Build each requested FlashSight UI variant sequentially.

xmake registers no project-local tasks, and one target graph can link only one
UI host (web / WebView / WhaleUI are different executables), so this script is
the "compile the selected variants" entry point:

    python scripts/build_all.py --web --webview
    python scripts/build_all.py --web --webview --whaleui -- --toolchain=mingw \
        --mingw="C:/Program Files/mingw64"

Anything after `--` is passed to every `xmake f`.
"""
import argparse
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--web", action="store_true")
    parser.add_argument("--webview", action="store_true")
    parser.add_argument("--whaleui", action="store_true")
    parser.add_argument("extra", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    variants = []
    if args.web:
        variants.append("web")
    if args.webview:
        variants.append("webview")
    if args.whaleui:
        variants.append("whaleui")
    if not variants:
        variants = ["web", "webview"]  # historical release set
    extra = list(args.extra)
    if extra and extra[0] == "--":
        extra = extra[1:]
    for variant in variants:
        print("[build-all] configuring", variant, flush=True)
        subprocess.check_call(
            ["xmake", "f", "-y", "--variant=" + variant] + extra
        )
        print("[build-all] building", variant, flush=True)
        subprocess.check_call(["xmake", "-y"])
    print("[build-all] ok:", ", ".join(variants), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
