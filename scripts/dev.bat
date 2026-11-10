@echo off
rem Build and tests at idle priority (see build.bat). UC_LOW_PRIORITY=0 turns it off.
if not defined UC_LOW_PRIORITY (
  set "UC_LOW_PRIORITY=1"
  start "" /b /low /wait cmd /c ""%~f0" %*"
  exit /b
)
setlocal
set "ROOT=%~dp0.."
call "%~dp0env.bat" || exit /b 1
cmake -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_THINLTO=ON --log-level=WARNING || exit /b 1
cmake --build "%ROOT%\build" || exit /b 1
if not defined CTEST_PARALLEL_LEVEL set "CTEST_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"
if not defined CTEST_PARALLEL_LEVEL set "CTEST_PARALLEL_LEVEL=1"
echo Running tests with up to %CTEST_PARALLEL_LEVEL% parallel jobs
rem Without arguments only the tests whose inputs changed since they last passed run (scripts\tests-run.ps1).
rem Extra ctest arguments run plain ctest.
if not "%~1"=="" goto :plain_ctest
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tests-run.ps1" || exit /b 1
exit /b 0
:plain_ctest
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ctest-hidden.ps1" %* || exit /b 1
exit /b 0
