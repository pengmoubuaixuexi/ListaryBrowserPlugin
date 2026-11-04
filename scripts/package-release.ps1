param(
    [string]$AppVersion = '2.2.0'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$release = Join-Path $root 'build\Release'
$portable = Join-Path $root 'build\portable'
$artifacts = Join-Path $root 'build\artifacts'
if (-not (Test-Path -LiteralPath (Join-Path $release 'BrowserHistoryLauncher.exe'))) {
    & (Join-Path $PSScriptRoot 'build-release.ps1')
}
function Get-BinaryVersion([string]$Path) {
    $info = (Get-Item -LiteralPath $Path).VersionInfo
    return [version]::new($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart)
}
$expectedVersion = [version]($AppVersion + '.0')
if ((Get-BinaryVersion (Join-Path $release 'BrowserHistoryLauncher.exe')) -ne $expectedVersion) {
    throw "Build the matching $AppVersion application before packaging."
}
$installer = Join-Path $root 'build\installer\ListaryBrowserPlugin-Setup-x64.exe'
if ((Test-Path -LiteralPath $installer) -and
    (Get-BinaryVersion $installer) -ne $expectedVersion) {
    throw "Build the matching $AppVersion installer before packaging; refusing to include an older installer."
}
if (Test-Path -LiteralPath $portable) {
    $resolvedPortable = (Resolve-Path -LiteralPath $portable).Path
    $expectedPortable = [IO.Path]::GetFullPath((Join-Path $root 'build\portable'))
    if ($resolvedPortable -ne $expectedPortable -or
        ((Get-Item -LiteralPath $resolvedPortable).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Refusing to replace an unexpected portable directory.'
    }
    # Preserve the previous generated package instead of recursively deleting it.
    $portableBackup = Join-Path $root ('build\portable-backup-' + [Guid]::NewGuid().ToString('N'))
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($portableBackup)) -ne
        [IO.Path]::GetFullPath((Join-Path $root 'build'))) {
        throw 'Portable backup must remain inside the build directory.'
    }
    Move-Item -LiteralPath $resolvedPortable -Destination $portableBackup
}
New-Item -ItemType Directory -Path $portable -Force | Out-Null
$portableDocs = Join-Path $portable 'docs'
New-Item -ItemType Directory -Path $portableDocs -Force | Out-Null
$portableScripts = Join-Path $portable 'scripts'
New-Item -ItemType Directory -Path $portableScripts -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $release 'BrowserHistoryLauncher.exe') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'BrowserHistoryLauncher.ini') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'CHANGELOG.md') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $portable -Force
# Keep the root-level report for compatibility with packages produced before the docs folder existed.
Copy-Item -LiteralPath (Join-Path $root 'docs\RESOURCE_REPORT.md') -Destination $portable -Force
Copy-Item -LiteralPath (Join-Path $root 'docs\RESOURCE_REPORT.md') -Destination $portableDocs -Force
Copy-Item -LiteralPath (Join-Path $root 'docs\LISTARY_INTEGRATION.md') -Destination $portableDocs -Force
Copy-Item -LiteralPath (Join-Path $root 'docs\EXE_TEST_2026-10-17.md') -Destination $portableDocs -Force
Copy-Item -LiteralPath (Join-Path $root 'scripts\configure-startup.ps1') -Destination $portableScripts -Force
Copy-Item -LiteralPath (Join-Path $root 'scripts\setup-current-user.ps1') -Destination $portableScripts -Force
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
$portableZip = Join-Path $artifacts "BrowserHistoryLauncher-v$AppVersion-portable-x64.zip"
if (Test-Path -LiteralPath $portableZip) {
    Remove-Item -LiteralPath $portableZip -Force
}
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath $portableZip -CompressionLevel Optimal
$releaseFiles = @($portableZip)
$installer = Join-Path $root 'build\installer\ListaryBrowserPlugin-Setup-x64.exe'
if (Test-Path -LiteralPath $installer -PathType Leaf) {
    $publishedInstaller = Join-Path $artifacts 'ListaryBrowserPlugin-Setup-x64.exe'
    Copy-Item -LiteralPath $installer -Destination $publishedInstaller -Force
    $releaseFiles += $publishedInstaller
}
$checksums = $releaseFiles | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
    '{0} *{1}' -f $hash.Hash.ToLowerInvariant(), (Split-Path $_ -Leaf)
}
$checksumsPath = Join-Path $artifacts 'SHA256SUMS.txt'
$checksums | Set-Content -LiteralPath $checksumsPath -Encoding ascii
Get-ChildItem -LiteralPath $portable | Select-Object Name, Length
Get-Item -LiteralPath @($releaseFiles + $checksumsPath) | Select-Object FullName, Length, LastWriteTime
