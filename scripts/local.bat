@echo off
rem Loads local.env from the project root (KEY=VALUE lines, "#" comments) into the environment of the caller. Variables that are already set win.
rem Call it without setlocal in between: call "%~dp0local.bat"
if not exist "%~dp0..\local.env" exit /b 0
for /f "usebackq eol=# tokens=1,* delims==" %%a in ("%~dp0..\local.env") do if not defined %%a set "%%a=%%b"
exit /b 0
