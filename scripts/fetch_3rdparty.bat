@echo off
rem FlashSight 3rdparty fetch (wrapper around fetch_3rdparty.py).
rem
rem   scripts\fetch_3rdparty.bat
rem   set PROXY=http://127.0.0.1:10809 ^&^& scripts\fetch_3rdparty.bat
rem   set SKIP_WEBVIEW=1 ^&^& scripts\fetch_3rdparty.bat
rem
rem The real implementation is Python so one script covers every host.
setlocal
cd /d "%~dp0.."
python scripts\fetch_3rdparty.py %*
exit /b %ERRORLEVEL%
