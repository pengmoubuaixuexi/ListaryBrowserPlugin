param(
    [string]$AppVersion = '2.0.0'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'build-release.ps1')

$compilerCandidates = @(
    $env:ISCC_PATH,
    (Join-Path ${env:ProgramFiles} 'Inno Setup 7\ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 7\ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path ${env:ProgramFiles} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

$compiler = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $compiler) {
    throw 'Inno Setup ISCC.exe was not found. Install Inno Setup 6/7 or set ISCC_PATH.'
}

$installerScript = Join-Path $projectRoot 'installer\BrowserHistoryLauncher.iss'
& $compiler "/DAppVersion=$AppVersion" $installerScript
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE"
}

$output = Join-Path $projectRoot 'build\installer\ListaryBrowserPlugin-Setup-x64.exe'
if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
    throw "Installer output was not created: $output"
}
Get-Item -LiteralPath $output | Select-Object FullName, Length, LastWriteTime
