# Runs only the tests whose inputs changed since they last passed (state: build\tests.state.json, delete it to run all).
# UC_ALL_TESTS=1 runs every test regardless of the state; scripts\build_release.py sets it.
# A test's key covers its command line, the files that command starts, the helper exes and dlls in build\tests,
# and every runtime data file (tests data and fixtures, tools, modding, mods-examples). Code of the other folders
# reaches a test through its relinked exe. Any failure to compute keys runs every test.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$build = Join-Path $root 'build'
$statePath = Join-Path $build 'tests.state.json'

function Get-Hash([string]$text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    [System.BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text))).Replace('-', '')
}
function Get-Stat([string]$path) {
    $f = [System.IO.FileInfo]::new($path)
    if ($f.Exists) { "$path|$($f.Length)|$($f.LastWriteTimeUtc.Ticks)" } else { "$path|gone" }
}

$keys = @{}
try {
    $tests = (ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json).tests
    if ($LASTEXITCODE -ne 0 -or -not $tests) { throw 'ctest list failed' }
    $data = (git ls-files -z -c -o --exclude-standard -- tests tools modding mods-examples) -split "`0" |
        Where-Object { $_ -and -not ($_ -match '^tests/([^/]+\.(cpp|h)|CMakeLists\.txt)$') }
    if ($LASTEXITCODE -ne 0 -or -not $data) { throw 'git list failed' }
    $shared = Get-Hash (($data | ForEach-Object { Get-Stat (Join-Path $root $_) }) -join "`n")
    $helpers = [System.IO.DirectoryInfo]::new((Join-Path $build 'tests')).EnumerateFiles() |
        Where-Object { $_.Extension -eq '.dll' -or ($_.Extension -eq '.exe' -and $_.Name -notlike 'test_*') } |
        ForEach-Object { Get-Stat $_.FullName }
    $helperHash = Get-Hash ($helpers -join "`n")
    foreach ($t in $tests) {
        $parts = @($shared, $helperHash) + @($t.command | ForEach-Object { $_ })
        $parts += @($t.command | Where-Object { $_ -and [System.IO.File]::Exists($_) } | ForEach-Object { Get-Stat $_ })
        $keys[$t.name] = Get-Hash ($parts -join "`n")
    }
} catch {
    Write-Host "Test keys unavailable ($($_.Exception.Message)); running all tests"
    $keys = @{}
}

$state = @{}
if ($keys.Count -and (Test-Path $statePath)) {
    try { (Get-Content $statePath -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $state[$_.Name] = $_.Value } } catch { $state = @{} }
}
$pending = @($keys.Keys | Where-Object { $env:UC_ALL_TESTS -or $state[$_] -ne $keys[$_] } | Sort-Object)
if ($keys.Count -and -not $pending) {
    Write-Host "Tests skipped: none of the $($keys.Count) tests changed since their last pass"
    exit 0
}

$ctestArgs = @('--test-dir', $build, '--output-on-failure')
if ($keys.Count -and -not $env:UC_ALL_TESTS) {
    Write-Host "Running $($pending.Count) of $($keys.Count) tests whose inputs changed"
    $ctestArgs += @('-R', ('^(' + (($pending | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'))
}
$junit = Join-Path $build 'tests-junit.xml'
Remove-Item $junit -ErrorAction SilentlyContinue
$ctestArgs += @('--output-junit', $junit)
& ctest @ctestArgs
$code = $LASTEXITCODE

# Record every test that passed, whether or not others failed; failed and skipped ones stay pending.
if ($keys.Count -and (Test-Path $junit)) {
    foreach ($case in ([xml](Get-Content $junit -Raw)).testsuite.testcase) {
        if ($case.status -eq 'run' -and -not $case.failure -and -not $case.skipped -and $keys.ContainsKey($case.name)) { $state[$case.name] = $keys[$case.name] }
        else { $state.Remove($case.name) }
    }
    $out = @{}
    foreach ($name in $state.Keys) { if ($keys.ContainsKey($name)) { $out[$name] = $state[$name] } }
    $out | ConvertTo-Json | Set-Content $statePath
}
exit $code
