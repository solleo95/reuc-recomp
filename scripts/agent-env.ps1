# Codex 0.160.1 can pass both PATH and Path; .NET rejects that environment.
$taskSearchPath = [Environment]::GetEnvironmentVariable('PATH')
if ($null -eq $taskSearchPath) { throw 'Process PATH is missing' }
[Environment]::SetEnvironmentVariable('PATH', $null)
[Environment]::SetEnvironmentVariable('Path', $null)
[Environment]::SetEnvironmentVariable('PATH', $taskSearchPath)
if (-not [Environment]::GetEnvironmentVariable('CLAUDE_CONFIG_DIR')) {
    $taskProfile = [Environment]::GetEnvironmentVariable('USERPROFILE')
    if (-not $taskProfile) { throw 'USERPROFILE is missing' }
    [Environment]::SetEnvironmentVariable('CLAUDE_CONFIG_DIR', (Join-Path $taskProfile '.claude'))
}
