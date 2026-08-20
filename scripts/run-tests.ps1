$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'build-release.ps1')
$root = Split-Path $PSScriptRoot -Parent
$core = Join-Path $root 'build\tests\BrowserHistoryLauncher.CoreTests.exe'
$ui = Join-Path $root 'build\tests\BrowserHistoryLauncher.UiSmokeTests.exe'
$app = Join-Path $root 'build\Release\BrowserHistoryLauncher.exe'
$ini = Join-Path $root 'BrowserHistoryLauncher.ini'

& $core $ini
$coreExit = $LASTEXITCODE
& $ui $app
$uiExit = $LASTEXITCODE
& (Join-Path $PSScriptRoot 'test-bluetooth.ps1') -Executable $app
$bluetoothExit = $LASTEXITCODE
if ($coreExit -ne 0 -or $uiExit -ne 0 -or $bluetoothExit -ne 0) {
    throw "Acceptance tests failed. core=$coreExit ui=$uiExit bluetooth=$bluetoothExit"
}
