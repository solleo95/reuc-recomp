@echo off
setlocal
where py >nul 2>nul
if not errorlevel 1 (
  py -3 "%~dp0dump_text.py" %*
) else (
  python "%~dp0dump_text.py" %*
)
exit /b %errorlevel%
