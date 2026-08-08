$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root 'build\Release'
$portable = Join-Path $root 'build\portable'
if (-not (Test-Path -LiteralPath (Join-Path $release 'BrowserHistoryLauncher.exe'))) {
    & (Join-Path $PSScriptRoot 'build-release.ps1')
}
New-Item -ItemType Directory -Path $portable -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $release 'BrowserHistoryLauncher.exe') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'BrowserHistoryLauncher.ini') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'docs\RESOURCE_REPORT.md') -Destination $portable -Force
Get-ChildItem -LiteralPath $portable | Select-Object Name, Length
