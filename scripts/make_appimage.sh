#!/usr/bin/env bash
# Build a minimal AppImage for one FlashSight variant.
#
#   scripts/make_appimage.sh <web|webview> <exe> <out> <version> <x86_64|aarch64>
#
# The AppImage carries only the application files (executable, desktop entry,
# icon, README). GTK4/WebKitGTK 6.0 are system libraries, exactly like WebView2
# on Windows; bundling WebKit's helper processes into an AppImage is not
# reliable, so README.txt states the packages the host must provide.
set -euo pipefail

variant="${1:?variant}"
exe="${2:?exe path}"
out="${3:?output path}"
version="${4:-0.1.0}"
arch="${5:?x86_64 or aarch64}"
# xmake/matrix calls the 64-bit ARM architecture "arm64"; AppImage names it
# "aarch64". Normalize so the runner's matrix value cannot produce a 404.
case "$arch" in
  arm64|arm64-v8a) arch=aarch64 ;;
  x64|amd64) arch=x86_64 ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
appdir="$work/FlashSight.AppDir"
mkdir -p "$appdir/usr/bin"

appname="flashsight-${variant}"
cp "$exe" "$appdir/usr/bin/$appname"
chmod +x "$appdir/usr/bin/$appname"
cp "$root/packaging/flashsight.svg" "$appdir/flashsight.svg"

cat > "$appdir/$appname.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=FlashSight ($variant)
Comment=Coding agent desktop
Exec=$appname
Icon=flashsight
Categories=Development;
Terminal=false
EOF

cat > "$appdir/AppRun" <<EOF
#!/bin/sh
HERE="\$(dirname "\$(readlink -f "\$0")")"
exec "\$HERE/usr/bin/$appname" "\$@"
EOF
chmod +x "$appdir/AppRun"

cat > "$appdir/README.txt" <<EOF
FlashSight $version ($variant, $arch)

This AppImage contains the application only. The native window variant needs
the host's GTK4 + WebKitGTK 6.0 libraries:
  Debian/Ubuntu: sudo apt install libgtk-4-1 libwebkitgtk-6.0-4
The Web variant only needs a browser; it serves the UI itself.

Gemini Code Assist plan login needs FLASHSIGHT_GOOGLE_CLIENT_SECRET in the
environment (the client secret is intentionally not shipped).

Run: ./$(basename "$out") [--port N] [--root PATH]
EOF

tool="$work/appimagetool-$arch.AppImage"
# AppImageKit's continuous appimagetool has no aarch64 asset (404); the
# standalone AppImage/appimagetool repo publishes both x86_64 and aarch64.
url="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$arch.AppImage"
if command -v curl >/dev/null 2>&1; then
  curl -fsSL "$url" -o "$tool"
else
  wget -q "$url" -O "$tool"
fi
chmod +x "$tool"
mkdir -p "$(dirname "$out")"
ARCH="$arch" "$tool" --appimage-extract-and-run --no-appstream "$appdir" "$out"
echo "wrote $out"
