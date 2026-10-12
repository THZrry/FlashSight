#!/usr/bin/env bash
# FlashSight 3rdparty 获取脚本（setup 一次性，之后 3rdparty 不在 git 内）
# 用法: bash scripts/fetch_3rdparty.sh  （或手动照抄命令）
set -e
PROXY="${PROXY:-http://127.0.0.1:10809}"
CURL="curl -sL -x $PROXY --max-time 60"
mkdir -p 3rdparty

$CURL -o /tmp/ghfs.tgz https://github.com/gulrak/filesystem/archive/refs/tags/v1.5.14.tar.gz
tar -xzf /tmp/ghfs.tgz -C 3rdparty

$CURL -o 3rdparty/json/json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
mkdir -p 3rdparty/json

$CURL -o /tmp/dtl.zip https://github.com/cubicdaiya/dtl/archive/refs/tags/v1.20.zip
unzip -q -o /tmp/dtl.zip -d 3rdparty

$CURL -o /tmp/md4c.zip https://github.com/mity/md4c/archive/refs/heads/master.zip
unzip -q -o /tmp/md4c.zip -d 3rdparty

$CURL -o /tmp/httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
mkdir -p 3rdparty/httplib
cp /tmp/httplib.h 3rdparty/httplib/httplib.h

$CURL -o /tmp/mbedtls.zip https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.4.zip
unzip -q -o /tmp/mbedtls.zip -d 3rdparty
(cd 3rdparty/mbedtls-3.6.4 && git clone -q --depth 1 https://github.com/Mbed-TLS/mbedtls-framework.git framework)

# dtl MinGW patch: enableTrivial() const assigns member
python - <<'PY'
p = '3rdparty/dtl-1.20/dtl/Diff.hpp'
d = open(p,'rb').read()
old = b'bool               trivial;'
new = b'mutable bool      trivial;'
assert d.count(old)==1, d.count(old)
open(p,'wb').write(d.replace(old,new))
print('dtl patched')
PY

echo "3rdparty fetch done"
# --- qjs regex engine (QuickJS, MIT, Fabrice Bellard) ---
mkdir -p 3rdparty/qjs-regex
curl -sL -x $PROXY --max-time 120 -o qjs.tar.xz https://bellard.org/quickjs/quickjs-2024-01-13.tar.xz
tar -xf qjs.tar.xz -C temp
cp temp/quickjs-2024-01-13/{libregexp.c,libunicode.c,cutils.h,libregexp.h,libunicode.h,libregexp-opcode.h,libunicode-table.h} 3rdparty/qjs-regex/ 2>/dev/null || true
rm -f qjs.tar.xz
# --- reproc (subprocess management; rules/user.md) ---
# only the C sources + headers; xmake.lua picks the platform files per OS
mkdir -p 3rdparty/reproc/src 3rdparty/reproc/include/reproc
curl -sL -x $PROXY --max-time 120 -o reproc.tar.gz https://github.com/DaanDeMeyer/reproc/archive/refs/tags/v14.2.5.tar.gz
tar -xzf reproc.tar.gz -C temp
cp temp/reproc-14.2.5/reproc/include/reproc/*.h 3rdparty/reproc/include/reproc/
cp temp/reproc-14.2.5/reproc/src/*.c temp/reproc-14.2.5/reproc/src/*.h 3rdparty/reproc/src/
cp temp/reproc-14.2.5/LICENSE 3rdparty/reproc/LICENSE
rm -f reproc.tar.gz
echo "3rdparty fetch done"
