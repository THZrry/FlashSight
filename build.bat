@echo off
rem ===========================================================================
rem FlashSight build entry points.
rem
rem   build.bat web        -> Web variant      (browser tab + local HTTP server)
rem   build.bat webview    -> Webview variant  (native window via webview)
rem   build.bat whaleui    -> WhaleUI variant  (native window via WhaleUI)
rem   build.bat test       -> unit tests (Web config)
rem
rem All three variants share the same UI code and only swap the DOM backend
rem under IDom; all three also embed the local HTTP server (remote access is
rem still TODO). Each variant builds into its OWN subtree of build/ so they can
rem coexist:  build\web  build\webview  build\whaleui
rem
rem Requirements:
rem   * Web      : emcc on PATH (the wasm bundle) + a C++ toolchain
rem   * Webview  : 3rdparty/webview + 3rdparty/mswebview2
rem                (scripts\fetch_3rdparty.bat pulls them)
rem   * WhaleUI  : a built WhaleUI library (see doc-internal/ui-whaleui.md)
rem ===========================================================================
setlocal
cd /d "%~dp0"

set MODE=%1
if "%MODE%"=="" set MODE=web

if /i "%MODE%"=="web" goto web
if /i "%MODE%"=="webview" goto webview
if /i "%MODE%"=="whaleui" goto whaleui
if /i "%MODE%"=="test" goto test
echo unknown mode "%MODE%"  (web ^| webview ^| whaleui ^| test)
exit /b 1

:web
echo [build] Web variant (browser + local HTTP server)
xmake f -y --toolchain=mingw -o build\web
if errorlevel 1 exit /b 1
xmake -y
exit /b %ERRORLEVEL%

:webview
echo [build] Webview variant (native window via webview)
xmake f -y --toolchain=mingw -o build\webview --webview=y
if errorlevel 1 exit /b 1
xmake -y
exit /b %ERRORLEVEL%

:whaleui
echo [build] WhaleUI variant (native window via WhaleUI)
xmake f -y --toolchain=mingw -o build\whaleui --whaleui=y ^
      --whaleui_libdir=C:/aaa/WhaleUI/build/mingw/x86_64/release
if errorlevel 1 exit /b 1
xmake -y
exit /b %ERRORLEVEL%

:test
echo [build] tests (Web config)
xmake f -y --toolchain=mingw -o build\web
if errorlevel 1 exit /b 1
xmake -y
if errorlevel 1 exit /b 1
build\web\windows\x64\release\fs_tests.exe
exit /b %ERRORLEVEL%
