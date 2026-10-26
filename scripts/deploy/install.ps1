param([Parameter(Mandatory)][string]$Stage,[Parameter(Mandatory)][string]$Suffix)
# Copies the files listed in $Stage\changed.json (relative to dist\) and $Stage\release.json to dist-raw, dist-release and the laptop.
# All targets are checked for locks and backed up (.before-$Suffix) first; any failure restores what was replaced.
$ErrorActionPreference = 'Stop'
$root = 'D:\Projects\Native Ports\Umbrella-chronicles-recomp'
$destinations = @(
    @{ Root = (Join-Path $root 'dist-raw'); Remote = $false },
    @{ Root = (Join-Path $root 'dist-release'); Remote = $false },
    @{ Root = '\\192.168.50.170\dist-raw'; Remote = $true }
)
$changed = @(Get-Content -Raw (Join-Path $Stage 'changed.json') | ConvertFrom-Json)
$files = @(foreach ($relative in $changed) {
    $name = $relative -replace '/','\'
    @{ Source = (Join-Path $root "dist\$name"); Relative = $name; Temp = ($name -replace '\\','_') + ".$Suffix.new" }
}) + @(@{ Source = (Join-Path $Stage 'release.json'); Relative = 'release.json'; Temp = "release.$Suffix.new" })
$replacements = [System.Collections.Generic.List[object]]::new()
foreach ($destination in $destinations) {
    foreach ($item in $files) {
        $target = Join-Path $destination.Root $item.Relative
        $backup = "$target.before-$Suffix"
        if (Test-Path -LiteralPath $backup) { throw "Backup already exists: $backup" }
        $handle = [IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        $handle.Dispose()
        $replacements.Add(@{ Target=$target; Backup=$backup; Temp=(Join-Path $destination.Root $item.Temp); Expected=(Get-FileHash -LiteralPath $item.Source -Algorithm SHA256).Hash })
    }
    if ($destination.Remote) {
        $incoming = Join-Path $Stage 'incoming'
        New-Item -ItemType Directory -Force -Path $incoming | Out-Null
        foreach ($item in $files) { Copy-Item -LiteralPath $item.Source -Destination (Join-Path $incoming $item.Temp) -Force }
        & robocopy $incoming $destination.Root "*.$Suffix.new" /Z /COMPRESS /R:2 /W:2 /NP /NFL /NDL | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Laptop transfer failed: $LASTEXITCODE" }
    } else {
        foreach ($item in $files) { Copy-Item -LiteralPath $item.Source -Destination (Join-Path $destination.Root $item.Temp) -Force }
    }
}
foreach ($item in $replacements) {
    if ((Get-FileHash -LiteralPath $item.Temp -Algorithm SHA256).Hash -ne $item.Expected) { throw "Staged hash mismatch: $($item.Temp)" }
}
$installed = [System.Collections.Generic.List[object]]::new()
try {
    foreach ($item in $replacements) {
        [IO.File]::Replace($item.Temp,$item.Target,$item.Backup,$true)
        $installed.Add($item)
    }
    foreach ($item in $installed) {
        if ((Get-FileHash -LiteralPath $item.Target -Algorithm SHA256).Hash -ne $item.Expected) { throw "Installed hash mismatch: $($item.Target)" }
        "Verified: $($item.Target)"
    }
} catch {
    $failure = $_
    for ($index=$installed.Count-1; $index -ge 0; $index--) {
        $item = $installed[$index]
        try { [IO.File]::Replace($item.Backup,$item.Target,$null,$true) }
        catch { Write-Warning "Rollback failed for $($item.Target): $_" }
    }
    throw $failure
}

# robocopy leaves LASTEXITCODE 1 ("files copied"), which the caller would read as a failure.
$global:LASTEXITCODE = 0
