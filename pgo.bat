@echo off
rem Records the optimizer profiles (PGO) for the release build. Run it on the FINAL tree, before build.bat / the release scripts:
rem the profile belongs to the source it was recorded from, so any code change afterwards makes it partly stale.
rem
rem   pgo.bat            both profiles
rem   pgo.bat windows    project\pgo\uc.profdata        (the Windows build; about an hour: instrumented build, harness scenarios)
rem   pgo.bat linux      project\pgo\uc-linux.profdata  (the Steam Deck build; about 45 minutes: instrumented build, three demos on the Deck)
rem
rem Windows needs the game files (UC_INSTALL_DIR in local.env, as for build.bat), nothing else running on the desktop and the F5 mark of the fight scenario
rem (scripts\uctest\marks\fight.json, python -m uctest run fight --mark). Linux needs the Steam Deck on the network (ssh deck, no game running on it) and
rem UC_DECK_SHORTCUT (the Steam shortcut id of the game on the Deck) in the environment; UC_DECK_DIR defaults to ~/Games/UmbrellaChronicles.
rem The profiles are local files (project\pgo is not in git). Build afterwards: build.bat for Windows, scripts\linux\build.ps1 -Target uc for the Deck (pgo.bat linux does the last).
rem Details: docs\testing\performance-measurement.md, section PGO.
setlocal EnableExtensions
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%" || exit /b 1
set "WHAT=%~1"
if "%WHAT%"=="" set "WHAT=all"
if /I "%WHAT%"=="all" goto :windows
if /I "%WHAT%"=="windows" goto :windows
if /I "%WHAT%"=="linux" goto :linux
echo usage: pgo.bat [windows^|linux]   (no argument: both)
exit /b 1

:windows
echo === Windows profile ===
call "%ROOT%\scripts\local.bat"
python "%ROOT%\scripts\pgo.py" || exit /b 1
if /I "%WHAT%"=="windows" exit /b 0

:linux
echo === Steam Deck (Linux) profile ===
if not defined UC_DECK_SHORTCUT echo UC_DECK_SHORTCUT is not set: the Steam shortcut id of the game on the Deck.& exit /b 1
python "%ROOT%\scripts\linux\pgo_train.py" || exit /b 1
exit /b 0
