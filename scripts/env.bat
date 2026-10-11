@echo off
rem Sets up MSVC x64 + LLVM + Vulkan SDK. Returns 1 if anything is missing.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_vs
rem vcvars calls vswhere by bare name; put its folder on PATH to avoid a harmless "not recognized" message.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
rem Machine-level VULKAN_SDK may be missing in sessions started before the SDK install.
if not defined VULKAN_SDK for /f "tokens=2,*" %%a in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v VULKAN_SDK 2^>nul') do set "VULKAN_SDK=%%b"
where clang-cl >nul 2>nul || (echo ERROR: clang-cl not found. Install LLVM, see plan Task 1. & exit /b 1)
where cmake >nul 2>nul || (echo ERROR: cmake not found. & exit /b 1)
where ninja >nul 2>nul || (echo ERROR: ninja not found. & exit /b 1)
if not defined VULKAN_SDK (echo ERROR: VULKAN_SDK is not set. Install the Vulkan SDK, see plan Task 1. & exit /b 1)
if not exist "%VULKAN_SDK%\Bin\slangc.exe" (echo ERROR: slangc.exe not found in the Vulkan SDK. & exit /b 1)
exit /b 0
:no_vs
echo ERROR: Visual Studio Build Tools with the C++ workload were not found. See plan Task 1.
exit /b 1
