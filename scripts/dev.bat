@echo off
setlocal
set "ROOT=%~dp0.."
call "%~dp0env.bat" || exit /b 1
cmake -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build "%ROOT%\build" || exit /b 1
ctest --test-dir "%ROOT%\build" --output-on-failure %* || exit /b 1
exit /b 0
