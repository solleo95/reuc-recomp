@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "DIST=%ROOT%\dist"
set "GEN=%ROOT%\generated"
call "%ROOT%\scripts\local.bat"
if not defined UC_INSTALL_DIR goto :no_install_var
set "GAME_DOL=%UC_INSTALL_DIR%\game\DATA\sys\main.dol"
set "NO_INSTALL="
if /I "%~1"=="--no-install" set "NO_INSTALL=1"

rem Messages that print user paths live behind labels: a ")" in a path would break a ( ) block.
echo [1/7] Checking game files
if not exist "%UC_INSTALL_DIR%\" goto :no_install_dir
if not exist "%GAME_DOL%" goto :no_dol
call "%ROOT%\scripts\env.bat" || exit /b 1
if not exist "%BUILD%" mkdir "%BUILD%"

echo [2/7] Configure and build the translator
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_THINLTO=ON > "%BUILD%\configure.log" 2>&1
if errorlevel 1 goto :build_failed
cmake --build "%BUILD%" --target translator > "%BUILD%\translator_build.log" 2>&1
if errorlevel 1 goto :build_failed

echo [3/7] Translating the game (cached by DOL + translator + uc.cfg + hle.cfg hash)
call "%ROOT%\scripts\translate.bat" || goto :translate_failed

echo [4/7] Build everything and run tests
call "%ROOT%\scripts\dev.bat" || goto :build_failed

echo [5/7] Building the programs for CPUs without AVX2 (x86-64-v2): game, launcher, updater
cmake -S "%ROOT%" -B "%ROOT%\build-compat" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_THINLTO=ON -DUC_ARCH=x86-64-v2 --log-level=WARNING > "%BUILD%\compat_configure.log" 2>&1
if errorlevel 1 goto :build_failed
cmake --build "%ROOT%\build-compat" --target uc uc_launcher uc_updater > "%BUILD%\compat_build.log" 2>&1
if errorlevel 1 goto :build_failed

echo [6/7] Staging dist
if not exist "%DIST%" mkdir "%DIST%"
if not exist "%DIST%" exit /b 1
cmake --install "%BUILD%" --prefix "%DIST%" > "%BUILD%\install.log" 2>&1 || goto :stage_failed

if defined NO_INSTALL goto :skip_copy
echo [7/7] Copying dist to %UC_INSTALL_DIR%
robocopy "%DIST%" "%UC_INSTALL_DIR%" /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP
if errorlevel 8 goto :copy_failed
echo Done.
exit /b 0

:skip_copy
echo [7/7] Skipping copy (--no-install)
exit /b 0

:no_install_var
echo ERROR: UC_INSTALL_DIR is not set. Copy local.env.example to local.env and set it to the folder with game\DATA\sys\main.dol (docs\building.md).
exit /b 1
:no_install_dir
echo ERROR: install directory not found: %UC_INSTALL_DIR%
exit /b 1
:no_dol
echo ERROR: main.dol not found: %GAME_DOL%
exit /b 1
:build_failed
echo ERROR: build or tests failed (see logs in %BUILD%)
exit /b 1
:translate_failed
echo ERROR: translation step failed
exit /b 1
:stage_failed
echo ERROR: install to dist failed (see %BUILD%\install.log)
exit /b 1
:copy_failed
echo ERROR: robocopy failed
exit /b 1
