@echo off
rem FlashSight 3rdparty fetch (Windows) - run from repo root
set PROXY=http://127.0.0.1:10809
mkdir 3rdparty 2>nul
curl -sL -x %PROXY% --max-time 90 -o ghfs.tgz https://github.com/gulrak/filesystem/archive/refs/tags/v1.5.14.tar.gz
tar -xzf ghfs.tgz -C 3rdparty
del /q ghfs.tgz
mkdir 3rdparty\json 2>nul
curl -sL -x %PROXY% --max-time 90 -o 3rdparty\json\json.hpp https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
curl -sL -x %PROXY% --max-time 90 -o dtl.zip https://github.com/cubicdaiya/dtl/archive/refs/tags/v1.20.zip
powershell -NoProfile -Command "Expand-Archive -Path dtl.zip -DestinationPath 3rdparty -Force"
del /q dtl.zip
curl -sL -x %PROXY% --max-time 90 -o md4c.zip https://github.com/mity/md4c/archive/refs/heads/master.zip
powershell -NoProfile -Command "Expand-Archive -Path md4c.zip -DestinationPath 3rdparty -Force"
del /q md4c.zip
mkdir 3rdparty\httplib 2>nul
curl -sL -x %PROXY% --max-time 90 -o 3rdparty\httplib\httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
curl -sL -x %PROXY% --max-time 120 -o mbedtls.zip https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.4.zip
powershell -NoProfile -Command "Expand-Archive -Path mbedtls.zip -DestinationPath 3rdparty -Force"
del /q mbedtls.zip
cd 3rdparty\mbedtls-3.6.4
git clone -q --depth 1 https://github.com/Mbed-TLS/mbedtls-framework.git framework
cd ..\..
python -c "p='3rdparty/dtl-1.20/dtl/Diff.hpp'; d=open(p,'rb').read(); o=b'bool               trivial;'; n=b'mutable bool      trivial;'; assert d.count(o)==1; open(p,'wb').write(d.replace(o,n)); print('dtl patched')"
rem --- qjs regex engine (QuickJS, MIT, Fabrice Bellard) ---
rem regex 工具用：libregexp + libunicode；来源 quickjs 发行包（与 WhaleJS src 同源）
mkdir 3rdparty\qjs-regex 2>nul
curl -sL -x %PROXY% --max-time 120 -o qjs.zip https://bellard.org/quickjs/quickjs-2024-01-13.tar.xz
if errorlevel 1 curl -sL -x %PROXY% --max-time 120 -o qjs.tar.xz https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.5.0.tar.gz
tar -xf qjs.zip -C temp 2>nul
if exist temp\quickjs-2024-01-13 (
  copy /y temp\quickjs-2024-01-13\libregexp.c 3rdparty\qjs-regex\ >nul
  copy /y temp\quickjs-2024-01-13\libunicode.c 3rdparty\qjs-regex\ >nul
  copy /y temp\quickjs-2024-01-13\cutils.h 3rdparty\qjs-regex\ >nul
  copy /y temp\quickjs-2024-01-13\libregexp.h 3rdparty\qjs-regex\ >nul
  copy /y temp\quickjs-2024-01-13\libunicode.h 3rdparty\qjs-regex\ >nul
  copy /y temp\quickjs-2024-01-13\libregexp-opcode.h 3rdparty\qjs-regex\ >nul
  copy /y temp\quickjs-2024-01-13\libunicode-table.h 3rdparty\qjs-regex\ >nul
)
del /q qjs.zip qjs.tar.xz 2>nul
rem 如本机已有 WhaleJS：可直接 copy C:\aaa\WhaleJS\src\{libregexp.c,libunicode.c,cutils.h,libregexp.h,libunicode.h,libregexp-opcode.h,libunicode-table.h} 3rdparty\qjs-regex\
rem --- reproc (subprocess management; rules/user.md) ---
rem 只用 C 源码 + 头文件；平台文件由 xmake.lua 按 OS 选择
mkdir 3rdparty\reproc\src 2>nul
mkdir 3rdparty\reproc\include\reproc 2>nul
curl -sL -x %PROXY% --max-time 120 -o reproc.tar.gz https://github.com/DaanDeMeyer/reproc/archive/refs/tags/v14.2.5.tar.gz
tar -xzf reproc.tar.gz -C temp
copy /y temp\reproc-14.2.5\reproc\include\reproc\*.h 3rdparty\reproc\include\reproc\ >nul
copy /y temp\reproc-14.2.5\reproc\src\*.c 3rdparty\reproc\src\ >nul
copy /y temp\reproc-14.2.5\reproc\src\*.h 3rdparty\reproc\src\ >nul
copy /y temp\reproc-14.2.5\LICENSE 3rdparty\reproc\LICENSE >nul
del /q reproc.tar.gz
echo 3rdparty fetch done