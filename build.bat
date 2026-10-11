@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "DIST=%ROOT%\dist"
set "GEN=%ROOT%\generated"
if not defined UC_INSTALL_DIR set "UC_INSTALL_DIR=S:\Games\Umbrella Chronicles"
set "GAME_DOL=%UC_INSTALL_DIR%\game\DATA\sys\main.dol"
set "CFG=%ROOT%\project\uc.cfg"
set "TRANSLATOR=%BUILD%\tools\translator\translator.exe"
set "NO_INSTALL="
if /I "%~1"=="--no-install" set "NO_INSTALL=1"

rem Messages that print user paths live behind labels: a ")" in a path would break a ( ) block.
echo [1/6] Checking game files
if not exist "%UC_INSTALL_DIR%\" goto :no_install_dir
if not exist "%GAME_DOL%" goto :no_dol
call "%ROOT%\scripts\env.bat" || exit /b 1
if not exist "%BUILD%" mkdir "%BUILD%"

echo [2/6] Configure and build the translator
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl > "%BUILD%\configure.log" 2>&1
if errorlevel 1 goto :build_failed
cmake --build "%BUILD%" --target translator > "%BUILD%\translator_build.log" 2>&1
if errorlevel 1 goto :build_failed

echo [3/6] Translating the game (cached by DOL + translator + uc.cfg hash)
set "KEY="
for /f %%h in ('powershell -NoProfile -Command "$h = foreach($p in $env:GAME_DOL,$env:TRANSLATOR,$env:CFG){(Get-FileHash -LiteralPath $p).Hash}; $h -join ''"') do set "KEY=%%h"
if not defined KEY goto :hash_failed
set "OLD="
if exist "%GEN%\.key" set /p OLD=<"%GEN%\.key"
if /I "%KEY%"=="%OLD%" if exist "%GEN%\game_table.cpp" goto :cached
if exist "%GEN%" rmdir /s /q "%GEN%"
"%TRANSLATOR%" --dol "%GAME_DOL%" --cfg "%CFG%" --out "%GEN%" --shard 150 > "%BUILD%\translate.log" 2>&1
if errorlevel 1 goto :translate_failed
>"%GEN%\.key" echo %KEY%
type "%BUILD%\translate.log"
goto :built
:cached
echo generated\ is up to date

:built
echo [4/6] Build everything and run tests
call "%ROOT%\scripts\dev.bat" || goto :build_failed

echo [5/6] Staging dist
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" || exit /b 1
cmake --install "%BUILD%" --prefix "%DIST%" || goto :stage_failed

if defined NO_INSTALL goto :skip_copy
echo [6/6] Copying dist to %UC_INSTALL_DIR%
robocopy "%DIST%" "%UC_INSTALL_DIR%" /E /NFL /NDL /NJH /NJS /NP
if errorlevel 8 goto :copy_failed
echo Done.
exit /b 0

:skip_copy
echo [6/6] Skipping copy (--no-install)
exit /b 0

:no_install_dir
echo ERROR: install directory not found: %UC_INSTALL_DIR%
exit /b 1
:no_dol
echo ERROR: main.dol not found: %GAME_DOL%
exit /b 1
:build_failed
echo ERROR: build or tests failed (see logs in %BUILD%)
exit /b 1
:hash_failed
echo ERROR: could not hash inputs for the translate cache
exit /b 1
:translate_failed
echo ERROR: translation failed
type "%BUILD%\translate.log"
exit /b 1
:stage_failed
echo ERROR: install to dist failed
exit /b 1
:copy_failed
echo ERROR: robocopy failed
exit /b 1
