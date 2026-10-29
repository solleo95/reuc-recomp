@echo off
setlocal
set "ROOT=%~dp0.."
call "%~dp0env.bat" || exit /b 1
cmake -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_THINLTO=ON || exit /b 1
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
ctest --test-dir "%ROOT%\build" --output-on-failure %* || exit /b 1
exit /b 0
