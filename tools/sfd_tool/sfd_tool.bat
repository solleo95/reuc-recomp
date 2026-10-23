@echo off
setlocal
where py >nul 2>nul
if not errorlevel 1 (
  py -3 "%~dp0sfd_tool.py" %*
) else (
  python "%~dp0sfd_tool.py" %*
)
exit /b %errorlevel%
