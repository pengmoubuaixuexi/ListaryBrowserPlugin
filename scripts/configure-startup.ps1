param(
    [switch]$Remove,
    [string]$TargetPath
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$startupDirectory = [Environment]::GetFolderPath('Startup')
$shortcutName = 'Listary' + (-join @([char]0x6D4F, [char]0x89C8, [char]0x5668, [char]0x63D2, [char]0x4EF6)) + '.lnk'
$shortcutPath = Join-Path $startupDirectory $shortcutName

if ($Remove) {
    if (Test-Path -LiteralPath $shortcutPath) {
        Remove-Item -LiteralPath $shortcutPath -Force
        Write-Host "Removed startup shortcut: $shortcutPath"
    } else {
        Write-Host "Startup shortcut does not exist: $shortcutPath"
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($TargetPath)) {
    $packageTarget = Join-Path $projectRoot 'BrowserHistoryLauncher.exe'
    $repositoryTarget = Join-Path $projectRoot 'build\portable\BrowserHistoryLauncher.exe'
    if (Test-Path -LiteralPath $packageTarget -PathType Leaf) {
        $TargetPath = $packageTarget
    } else {
        $TargetPath = $repositoryTarget
    }
}

$targetPath = [IO.Path]::GetFullPath($TargetPath)
if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
    throw "Executable not found: $targetPath. Run scripts\package-release.ps1 first."
}

$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $targetPath
$shortcut.WorkingDirectory = Split-Path -Parent $targetPath
$shortcut.Description = 'Listary browser history plugin (Chrome / Edge)'
$shortcut.IconLocation = "$targetPath,0"
$shortcut.Save()

$savedShortcut = $shell.CreateShortcut($shortcutPath)
if ($savedShortcut.TargetPath -ne $targetPath) {
    throw "Shortcut validation failed. Actual target: $($savedShortcut.TargetPath)"
}

Write-Host "Configured current-user startup: $shortcutPath"
Write-Host "Target: $($savedShortcut.TargetPath)"
