@echo off
setlocal
where py >nul 2>nul
if not errorlevel 1 (
  py -3 "%~dp0extract_textures.py" %*
) else (
  python "%~dp0extract_textures.py" %*
)
exit /b %errorlevel%
