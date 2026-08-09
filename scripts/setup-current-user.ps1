param(
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$packageTarget = Join-Path $projectRoot 'BrowserHistoryLauncher.exe'
$repositoryTarget = Join-Path $projectRoot 'build\portable\BrowserHistoryLauncher.exe'
$targetPath = if (Test-Path -LiteralPath $packageTarget -PathType Leaf) {
    $packageTarget
} else {
    $repositoryTarget
}
$startupScript = Join-Path $PSScriptRoot 'configure-startup.ps1'

if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
    throw "Executable not found: $targetPath"
}

if ($Remove) {
    $protocolProcess = Start-Process -FilePath $targetPath -ArgumentList '--unregister-listary-protocol', '--quiet' -Wait -PassThru
    if ($protocolProcess.ExitCode -ne 0) {
        throw "Protocol unregistration failed with exit code $($protocolProcess.ExitCode)"
    }
    & $startupScript -Remove
    Start-Process -FilePath $targetPath -ArgumentList '--exit' -Wait | Out-Null
    Write-Host 'Removed current-user protocol registration and startup shortcut.'
    exit 0
}

$iniPath = Join-Path (Split-Path -Parent $targetPath) 'BrowserHistoryLauncher.ini'
if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) {
    throw "Configuration file not found beside executable: $iniPath"
}

$protocolProcess = Start-Process -FilePath $targetPath -ArgumentList '--register-listary-protocol', '--quiet' -Wait -PassThru
if ($protocolProcess.ExitCode -ne 0) {
    throw "Protocol registration failed with exit code $($protocolProcess.ExitCode)"
}

& $startupScript -TargetPath $targetPath
$running = @(Get-Process -Name 'BrowserHistoryLauncher' -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -eq $targetPath
})
if ($running.Count -eq 0) {
    Start-Process -FilePath $targetPath -WorkingDirectory (Split-Path -Parent $targetPath) -WindowStyle Hidden
}

Write-Host 'Current-user setup completed.'
Write-Host 'Next: configure the g/e web searches in Listary as documented in docs\LISTARY_INTEGRATION.md.'
