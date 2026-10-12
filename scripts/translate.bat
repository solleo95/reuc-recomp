@echo off
rem Generates project\hle.cfg and translates the game when the inputs changed. Returns 1 on failure.
rem Expects ROOT, BUILD, GEN, GAME_DOL in the environment and the translator already built.
setlocal EnableExtensions
set "CFG=%ROOT%\project\uc.cfg"
set "HLECFG=%ROOT%\project\hle.cfg"
set "TRANSLATOR=%BUILD%\tools\translator\translator.exe"
python -I "%ROOT%\tools\hle_cfg.py" "%ROOT%\project"
if errorlevel 1 goto :hle_failed
set "KEY="
for /f %%h in ('powershell -NoProfile -Command "$h = foreach($p in $env:GAME_DOL,$env:TRANSLATOR,$env:CFG,$env:HLECFG){(Get-FileHash -LiteralPath $p).Hash}; $h -join ''"') do set "KEY=%%h"
if not defined KEY goto :hash_failed
set "OLD="
if exist "%GEN%\.key" set /p OLD=<"%GEN%\.key"
if /I "%KEY%"=="%OLD%" if exist "%GEN%\game_table.cpp" goto :cached
if exist "%GEN%" rmdir /s /q "%GEN%"
"%TRANSLATOR%" --dol "%GAME_DOL%" --cfg "%CFG%" --cfg "%HLECFG%" --out "%GEN%" --shard 150 > "%BUILD%\translate.log" 2>&1
if errorlevel 1 goto :translate_failed
>"%GEN%\.key" echo %KEY%
type "%BUILD%\translate.log"
exit /b 0
:cached
echo generated\ is up to date
exit /b 0
:hle_failed
echo ERROR: hle.cfg generation failed
exit /b 1
:hash_failed
echo ERROR: could not hash inputs for the translate cache
exit /b 1
:translate_failed
echo ERROR: translation failed
type "%BUILD%\translate.log"
exit /b 1
