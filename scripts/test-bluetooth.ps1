param(
    [string]$Executable = '',
    [string[]]$ExpectedDevices = @('AirPods', 'MINOR III', 'BT5.4 Mouse'),
    [string]$ConnectName = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Executable) {
    $Executable = Join-Path $root 'build\Release\BrowserHistoryLauncher.exe'
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path

$existing = Get-Process -Name BrowserHistoryLauncher -ErrorAction SilentlyContinue
if ($existing) {
    throw 'A BrowserHistoryLauncher process is already running. Exit it before the isolated Bluetooth test.'
}

$enumerationFile = [IO.Path]::GetTempFileName()
$connectionFile = [IO.Path]::GetTempFileName()
$hostProcess = $null
try {
    $worker = Start-Process -FilePath $Executable -ArgumentList @(
        '--bluetooth-worker-enumerate', ('"' + $enumerationFile + '"')) -Wait -PassThru -WindowStyle Hidden
    if ($worker.ExitCode -ne 0) {
        throw "Bluetooth enumeration worker failed with exit code $($worker.ExitCode)."
    }
    $enumeration = Get-Content -Raw -Encoding UTF8 -LiteralPath $enumerationFile | ConvertFrom-Json
    if ($enumeration.error) {
        throw $enumeration.error
    }
    foreach ($device in $enumeration.devices) {
        Write-Output "DEVICE name=$($device.name) connected=$($device.connected) present=$($device.present) transport=$($device.transport)"
    }
    foreach ($name in $ExpectedDevices) {
        if ($enumeration.devices.name -notcontains $name) {
            throw "Expected paired Bluetooth device was not returned: $name"
        }
    }
    $duplicateNames = $enumeration.devices | Group-Object name | Where-Object Count -gt 1
    if ($duplicateNames) {
        throw "Physical-device de-duplication failed: $($duplicateNames.Name -join ', ')"
    }
    Write-Output "METRIC bluetooth_worker_enumeration_ms=$($enumeration.elapsedMs) devices=$($enumeration.devices.Count) private_working_set_bytes=$($enumeration.privateWorkingSetBytes) private_bytes=$($enumeration.privateBytes) threads=$($enumeration.threads) handles=$($enumeration.handles)"

    $hostProcess = Start-Process -FilePath $Executable -PassThru -WindowStyle Hidden
    $health = $null
    for ($attempt = 0; $attempt -lt 50 -and -not $health; $attempt++) {
        try {
            $health = Invoke-RestMethod -Uri 'http://127.0.0.1:32119/health' -TimeoutSec 1
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $health.ok) {
        throw 'Listary suggestion host did not become healthy.'
    }
    $suggestions = Invoke-RestMethod -Uri 'http://127.0.0.1:32119/suggest?prefix=ly&q=' -TimeoutSec 10
    foreach ($name in $ExpectedDevices) {
        if (-not ($suggestions[1] | Where-Object { $_ -like "$name*" })) {
            throw "Listary ly suggestions did not contain: $name"
        }
    }
    $filtered = Invoke-RestMethod -Uri 'http://127.0.0.1:32119/suggest?prefix=ly&q=air' -TimeoutSec 10
    if ($filtered[1].Count -ne 1 -or $filtered[1][0] -notlike 'AirPods*') {
        throw 'Listary ly filtering did not return only AirPods.'
    }
    $hostProcess.Refresh()
    Write-Output "METRIC bluetooth_host_working_set_bytes=$($hostProcess.WorkingSet64) private_bytes=$($hostProcess.PrivateMemorySize64) threads=$($hostProcess.Threads.Count) handles=$($hostProcess.HandleCount)"

    if ($ConnectName) {
        $diagnostic = Start-Process -FilePath $Executable -ArgumentList @(
            '--bluetooth-diagnostic-connect', ('"' + $ConnectName + '"'),
            ('"' + $connectionFile + '"'), '8000') -Wait -PassThru -WindowStyle Hidden
        if ($diagnostic.ExitCode -ne 0) {
            throw "Bluetooth connection diagnostic failed with exit code $($diagnostic.ExitCode)."
        }
        $connection = Get-Content -Raw -Encoding UTF8 -LiteralPath $connectionFile | ConvertFrom-Json
        Write-Output "CONNECT name=$ConnectName attempted=$($connection.attempted) request_accepted=$($connection.requestAccepted) confirmed=$($connection.connected) elapsed_ms=$($connection.elapsedMs) message=$($connection.message)"
    }
    Write-Output 'SUMMARY bluetooth_failures=0'
} finally {
    if ($hostProcess -and -not $hostProcess.HasExited) {
        Start-Process -FilePath $Executable -ArgumentList '--exit' -Wait -WindowStyle Hidden
        $hostProcess.WaitForExit(5000) | Out-Null
    }
    Remove-Item -LiteralPath $enumerationFile, $connectionFile -Force -ErrorAction SilentlyContinue
}
