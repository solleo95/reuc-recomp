# Cross-build the Linux (Steam Deck) port from Windows.   usage: pwsh scripts/linux/build.ps1 [-Target <cmake target>] [-Clean]
# Needs: the sniper sysroot (scripts/linux/extract_sysroot.py), LLVM, CMake and Ninja on PATH.
param([string[]]$Target = @(), [switch]$Clean, [switch]$KeepGoing, [switch]$ThinLTO, [string]$Tune = '', [switch]$PgoGenerate, [string]$Folder = '', [string]$CxxFlags = '', [switch]$NoPgo)   # -PgoGenerate: build-linux-pgogen, instrumented (scripts/linux/pgo.py)   # -Tune znver2: build-linux-<tune>, tuned for that CPU   # -ThinLTO: build-linux-lto with whole-program ThinLTO (slow link)   # -KeepGoing: build everything that can build and list all failures (ninja -k 0)
$ErrorActionPreference = 'Stop'
$Target = @($Target | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # `pwsh -File build.ps1 -Target a,b` passes one string "a,b"
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$build = Join-Path $root $(if ($PgoGenerate) { 'build-linux-pgogen' } elseif ($ThinLTO) { 'build-linux-lto' } else { "build-linux-$(if ($Tune) { $Tune } else { 'znver2' })" })   # the compiler flags are fixed when a build folder is created: one folder per tune (the default: the Deck's CPU)
if ($NoPgo -and -not $Folder) { $Folder = 'nopgo' }   # -NoPgo: build-linux-nopgo, no optimizer profile: what changes are developed and compared on; the profile is trained once, on the final tree (docs/testing/performance-measurement.md)
if ($Folder) { $build = Join-Path $root "build-linux-$Folder" }   # -Folder <name> -CxxFlags "<flags>": an experiment with extra compiler flags in a build folder of its own
$tuneArg = if ($Tune) { "-DUC_TUNE=$Tune" } else { '-DUC_TUNE=znver2' }
$flagArgs = @()
if ($NoPgo) { $flagArgs += '-DUC_PGO_LINUX_PROFILE=none' }
if ($CxxFlags) { $env:CXXFLAGS = $CxxFlags }   # CMake appends $CXXFLAGS to the toolchain's initial flags when it creates the cache (a -DCMAKE_CXX_FLAGS would replace them)
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
$lto = if ($ThinLTO) { 'ON' } else { 'OFF' }
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DUC_THINLTO=$lto" "-DUC_PGO_GENERATE=$(if ($PgoGenerate) { 'ON' } else { 'OFF' })" $tuneArg @flagArgs --log-level=WARNING --toolchain (Join-Path $root 'cmake\toolchains\linux-sniper.cmake')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$args = @('--build', $build)
if ($Target.Count) { $args += '--target'; $args += $Target }
if ($KeepGoing) { $args += '--'; $args += '-k'; $args += '0' }
cmake @args
exit $LASTEXITCODE
