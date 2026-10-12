@echo off
rem Build the full FlashSight UI flow (no hardcoded paths):
rem   1) wasm:   xmake -p wasm   -> web/fs_ui.{js,wasm}
rem   2) assets: python scripts/gen_assets.py -> src/ui/ui_assets_gen.hpp
rem   3) native: xmake -p windows -> flashsight.exe (serves embedded UI over HTTP)
rem Requires: emcc on PATH (emsdk_env), xmake, python.
setlocal
cd /d "%~dp0.."

where emcc >nul 2>&1 || (echo [ERR] emcc not on PATH ^(activate emsdk_env^) & exit /b 1)
where xmake >nul 2>&1 || (echo [ERR] xmake not on PATH & exit /b 1)
where python >nul 2>&1 || (echo [ERR] python not on PATH & exit /b 1)

echo === 1/3 wasm ===
xmake f -p wasm -y --toolchain=emcc || exit /b 1
xmake -r fs_ui_wasm || exit /b 1

echo === 2/3 embed assets ===
python scripts\gen_assets.py || exit /b 1

echo === 3/3 native ===
xmake f -p windows -y --toolchain=mingw || exit /b 1
xmake -r flashsight || exit /b 1

echo DONE: build\windows\x64\release\flashsight.exe
endlocal
