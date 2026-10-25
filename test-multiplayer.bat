@echo off
setlocal EnableExtensions
set "TEST_RESULT=1"
if not exist "%~dp0dist\uc.exe" goto :missing_build
py -3.13 --version >nul 2>&1
if errorlevel 1 goto :missing_python
pushd "%~dp0scripts"
if errorlevel 1 goto :done
echo Multiplayer harness: RTT 200 ms, jitter +/-20 ms per direction, 60-second soak.
echo Two muted 1280x720 game windows; isolated saves; automatic controls.
echo Guest CPU: host 26, client 28 - separate physical cores on this test machine.
echo Reports: build\autotest\netplay-...\report.json
py -3.13 -m uctest.netplay --speed 1 --latency-rtt 200 --different-saves --soak-seconds 60
set "TEST_RESULT=%ERRORLEVEL%"
popd
goto :done

:missing_build
echo ERROR: dist\uc.exe is missing. Run build.bat --no-install first.
goto :done

:missing_python
echo ERROR: Python 3.13 is required, including the Windows py launcher.

:done
echo.
if "%TEST_RESULT%"=="0" (echo Multiplayer test PASSED.) else (echo Multiplayer test FAILED. Exit code: %TEST_RESULT%)
pause
exit /b %TEST_RESULT%
