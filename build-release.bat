@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0" || exit /b 1
call "%~dp0scripts\env.bat" || exit /b 1
python -I "%~dp0scripts\build_release.py"
set "RELEASE_RESULT=%errorlevel%"
if not "%RELEASE_RESULT%"=="0" echo Release build failed. See the error above.
pause
exit /b %RELEASE_RESULT%
