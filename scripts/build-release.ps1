$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe not found. Install Visual Studio 2022 Build Tools.'
}
$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) {
    throw 'MSVC x64 Build Tools were not found.'
}
$msbuild = Join-Path $install 'MSBuild\Current\Bin\MSBuild.exe'
$projects = @(
    'BrowserHistoryLauncher.vcxproj',
    'BrowserHistoryLauncher.CoreTests.vcxproj',
    'BrowserHistoryLauncher.UiSmokeTests.vcxproj'
)
foreach ($project in $projects) {
    & $msbuild $project /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed: $project"
    }
}
