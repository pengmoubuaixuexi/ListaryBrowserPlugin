$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root 'build\Release'
$portable = Join-Path $root 'build\portable'
if (-not (Test-Path -LiteralPath (Join-Path $release 'BrowserHistoryLauncher.exe'))) {
    & (Join-Path $PSScriptRoot 'build-release.ps1')
}
New-Item -ItemType Directory -Path $portable -Force | Out-Null
$portableDocs = Join-Path $portable 'docs'
New-Item -ItemType Directory -Path $portableDocs -Force | Out-Null
$portableScripts = Join-Path $portable 'scripts'
New-Item -ItemType Directory -Path $portableScripts -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $release 'BrowserHistoryLauncher.exe') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'BrowserHistoryLauncher.ini') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $portable -Force
# Keep the root-level report for compatibility with packages produced before the docs folder existed.
Copy-Item -LiteralPath (Join-Path $root 'docs\RESOURCE_REPORT.md') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'docs\RESOURCE_REPORT.md') -Destination $portableDocs -Force
Copy-Item -LiteralPath (Join-Path $root 'docs\LISTARY_INTEGRATION.md') -Destination $portableDocs -Force
Copy-Item -LiteralPath (Join-Path $root 'scripts\configure-startup.ps1') -Destination $portableScripts -Force
Copy-Item -LiteralPath (Join-Path $root 'scripts\setup-current-user.ps1') -Destination $portableScripts -Force
Get-ChildItem -LiteralPath $portable | Select-Object Name, Length
