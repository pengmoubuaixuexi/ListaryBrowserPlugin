$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$profileRoot = Join-Path $env:APPDATA 'Listary\UserProfile'
$preferencesPath = Join-Path $profileRoot 'Settings\Preferences.json'
$backupRoot = Join-Path $projectRoot 'local-backup\Listary'
$backupImageRoot = Join-Path $backupRoot 'UserFiles\Images'

if (-not (Test-Path -LiteralPath $preferencesPath -PathType Leaf)) {
    throw "Listary preferences not found: $preferencesPath"
}

New-Item -ItemType Directory -Path $backupImageRoot -Force | Out-Null
Copy-Item -LiteralPath $preferencesPath -Destination (Join-Path $backupRoot 'Preferences.json') -Force

$preferences = Get-Content -LiteralPath $preferencesPath -Raw -Encoding utf8 | ConvertFrom-Json
$copiedIcons = @()
$customSearches = @($preferences.WebSearch.Items.Insertions) |
    ForEach-Object { $_.Item } |
    Where-Object { $_.Keyword -in @('g', 'e') }

foreach ($search in $customSearches) {
    $iconPath = $search.Icon.Path
    if ([string]::IsNullOrWhiteSpace($iconPath)) {
        continue
    }

    $iconFileName = [IO.Path]::GetFileName($iconPath)
    $sourceIcon = Join-Path $profileRoot "UserFiles\Images\$iconFileName"
    if (Test-Path -LiteralPath $sourceIcon -PathType Leaf) {
        Copy-Item -LiteralPath $sourceIcon -Destination (Join-Path $backupImageRoot $iconFileName) -Force
        $copiedIcons += $iconFileName
    }
}

$listaryVersion = $null
$listaryExe = Join-Path $env:ProgramFiles 'Listary\Listary.exe'
if (Test-Path -LiteralPath $listaryExe -PathType Leaf) {
    $listaryVersion = (Get-Item -LiteralPath $listaryExe).VersionInfo.ProductVersion
}

$manifest = [ordered]@{
    createdAt = (Get-Date).ToString('o')
    source = $profileRoot
    listaryVersion = $listaryVersion
    webSearchKeywords = @($customSearches | ForEach-Object { $_.Keyword })
    copiedIcons = $copiedIcons
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backupRoot 'manifest.json') -Encoding utf8

Write-Host "Listary configuration backed up to: $backupRoot"
Write-Host "Web search keywords: $(@($manifest.webSearchKeywords) -join ', ')"
Write-Host "Icon file count: $($copiedIcons.Count)"
