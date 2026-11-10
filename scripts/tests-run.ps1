# Runs only the tests whose inputs changed since they last passed (state: build\tests.state.json, delete it to run all).
# UC_ALL_TESTS=1 runs every test regardless of the state; scripts\build_release.py sets it.
# A test's key covers its command line, the files that command starts, the helper exes and dlls in build\tests,
# and every runtime data file (tests data and fixtures, tools, modding, mods-examples). Code of the other folders
# reaches a test through its relinked exe. Any failure to compute keys runs every test.
$ErrorActionPreference = 'Stop'
if ($env:UC_LOW_PRIORITY -ne '0') { try { [System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'Idle' } catch { } }   # the tests use every core: children inherit the class, a game being played keeps its speed
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

$ctestArgs = @('--test-dir', $build, '--quiet')   # --quiet: no line per test; the summary and the output of failed tests are printed below from the junit file
if ($keys.Count -and -not $env:UC_ALL_TESTS) {
    Write-Host "Running $($pending.Count) of $($keys.Count) tests whose inputs changed"
    $ctestArgs += @('-R', ('^(' + (($pending | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'))
}
$junit = Join-Path $build 'tests-junit.xml'
Remove-Item $junit -ErrorAction SilentlyContinue
$ctestArgs += @('--output-junit', $junit)
. (Join-Path $PSScriptRoot 'hidden-desktop.ps1')
$code = Invoke-OnHiddenDesktop 'ctest' $ctestArgs   # the tests open real windows: on another desktop they cannot pop up over what the user is doing (UC_TESTS_VISIBLE=1 shows them)

$retriedOk = @{}
if (Test-Path $junit) {
    $cases = @(([xml](Get-Content $junit -Raw)).testsuite.testcase)
    $failed = @($cases | Where-Object { $_.failure })
    $skipped = @($cases | Where-Object { $_.skipped }).Count
    if ($failed.Count) {   # at idle priority, on a busy machine, a test with a timeout can fail without a fault: the failed ones run once more, alone
        Write-Host "Running the $($failed.Count) failed test(s) again"
        $retryJunit = "$junit.retry"; Remove-Item $retryJunit -ErrorAction SilentlyContinue
        Invoke-OnHiddenDesktop 'ctest' @('--test-dir', $build, '--rerun-failed', '--output-on-failure', '--output-junit', $retryJunit) | Out-Null
        if (Test-Path $retryJunit) {
            foreach ($case in ([xml](Get-Content $retryJunit -Raw)).testsuite.testcase) { if ($case.status -eq 'run' -and -not $case.failure) { $retriedOk[$case.name] = $true } }
            $failed = @($failed | Where-Object { -not $retriedOk.ContainsKey($_.name) })
            if (-not $failed.Count) { $code = 0 }
        }
    }
    Write-Host "Tests: $($cases.Count - $failed.Count - $skipped) of $($cases.Count) passed$(if ($skipped) { ", $skipped skipped" })"
    foreach ($case in $failed) {
        Write-Host "FAILED: $($case.name)"
        $text = "$($case.'system-out'.InnerText)`n$($case.failure.InnerText)".Trim()
        if ($text) { Write-Host (($text -split "`r?`n" | Select-Object -Last 40) -join "`n") }
    }
} elseif ($code -ne 0) {
    Write-Host "ctest failed with code $code and wrote no junit file: run scripts\ctest-hidden.ps1 for its output"
}

# Record every test that passed, whether or not others failed; failed and skipped ones stay pending.
if ($keys.Count -and (Test-Path $junit)) {
    foreach ($case in ([xml](Get-Content $junit -Raw)).testsuite.testcase) {
        if ($case.status -eq 'run' -and (-not $case.failure -or $retriedOk.ContainsKey($case.name)) -and -not $case.skipped -and $keys.ContainsKey($case.name)) { $state[$case.name] = $keys[$case.name] }
        else { $state.Remove($case.name) }
    }
    $out = @{}
    foreach ($name in $state.Keys) { if ($keys.ContainsKey($name)) { $out[$name] = $state[$name] } }
    $out | ConvertTo-Json | Set-Content $statePath
}
exit $code
