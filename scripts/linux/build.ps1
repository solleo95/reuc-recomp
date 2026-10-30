# Cross-build the Linux (Steam Deck) port from Windows.   usage: pwsh scripts/linux/build.ps1 [-Target <cmake target>] [-Clean]
# Needs: the sniper sysroot (scripts/linux/extract_sysroot.py), LLVM, CMake and Ninja on PATH.
param([string[]]$Target = @(), [switch]$Clean)
$ErrorActionPreference = 'Stop'
$Target = @($Target | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # `pwsh -File build.ps1 -Target a,b` passes one string "a,b"
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$build = Join-Path $root 'build-linux'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release --toolchain (Join-Path $root 'cmake\toolchains\linux-sniper.cmake')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$args = @('--build', $build)
if ($Target.Count) { $args += '--target'; $args += $Target }
cmake @args
exit $LASTEXITCODE
