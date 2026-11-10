# ctest with the tests' windows on a hidden desktop: powershell -File scripts\ctest-hidden.ps1 <ctest arguments>  (the build folder is build\ next to scripts\).
$ErrorActionPreference = 'Stop'
if ($env:UC_LOW_PRIORITY -ne '0') { try { [System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'Idle' } catch { } }   # the tests use every core: children inherit the class, a game being played keeps its speed
. (Join-Path $PSScriptRoot 'hidden-desktop.ps1')
$build = Join-Path (Split-Path -Parent $PSScriptRoot) 'build'
exit (Invoke-OnHiddenDesktop 'ctest' (@('--test-dir', $build, '--output-on-failure') + $args))
