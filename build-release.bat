@echo off
rem The release build uses every core: run it at idle priority so it never slows a game being played. UC_LOW_PRIORITY=0 turns it off.
if not defined UC_LOW_PRIORITY (
  set "UC_LOW_PRIORITY=1"
  start "" /b /low /wait cmd /c ""%~f0" %*"
  exit /b
)
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0" || exit /b 1
call "%~dp0scripts\env.bat" || exit /b 1
python -I "%~dp0scripts\build_release.py"
set "RELEASE_RESULT=%errorlevel%"
if not "%RELEASE_RESULT%"=="0" echo Release build failed. See the error above.
pause
exit /b %RELEASE_RESULT%
