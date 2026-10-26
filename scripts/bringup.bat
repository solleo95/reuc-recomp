@echo off
rem Trace build of uc.exe in build-trace\ and a run. Usage: scripts\bringup.bat [uc.exe arguments]  (default: --seconds 20)
setlocal EnableExtensions
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "BUILD=%ROOT%\build"
set "GEN=%ROOT%\generated"
call "%~dp0local.bat"
if not defined UC_INSTALL_DIR (echo ERROR: UC_INSTALL_DIR is not set, see local.env.example & exit /b 1)
set "GAME_DOL=%UC_INSTALL_DIR%\game\DATA\sys\main.dol"
set "ARGS=%*"
if not defined ARGS set "ARGS=--seconds 20"
call "%~dp0env.bat" || exit /b 1
if not exist "%BUILD%" mkdir "%BUILD%"
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_THINLTO=ON > "%BUILD%\configure.log" 2>&1
if errorlevel 1 goto :fail
cmake --build "%BUILD%" --target translator > "%BUILD%\translator_build.log" 2>&1
if errorlevel 1 goto :fail
call "%~dp0translate.bat" || exit /b 1
if not exist "%ROOT%\build-trace" mkdir "%ROOT%\build-trace"
cmake -S "%ROOT%" -B "%ROOT%\build-trace" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_TRACE=ON > "%ROOT%\build-trace\configure.log" 2>&1
if errorlevel 1 goto :fail
cmake --build "%ROOT%\build-trace" --target uc > "%ROOT%\build-trace\build.log" 2>&1
if errorlevel 1 goto :fail
"%ROOT%\build-trace\game\uc.exe" --game "%UC_INSTALL_DIR%\game" %ARGS% 2> "%ROOT%\build-trace\run.log"
set "RC=%errorlevel%"
echo uc.exe exit code %RC%
powershell -NoProfile -Command "Get-Content -LiteralPath ($env:ROOT + '\build-trace\run.log') -Tail 60"
exit /b %RC%
:fail
echo ERROR: build failed, see the logs in %BUILD% and %ROOT%\build-trace
powershell -NoProfile -Command "Get-Content -LiteralPath ($env:ROOT + '\build-trace\build.log') -Tail 30 -ErrorAction SilentlyContinue"
exit /b 1
