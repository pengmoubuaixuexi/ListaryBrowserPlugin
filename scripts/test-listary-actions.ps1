param(
    [string]$Executable = '',
    [string]$CoreTestsExecutable = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Executable) { $Executable = Join-Path $root 'build\Release\BrowserHistoryLauncher.exe' }
if (-not $CoreTestsExecutable) { $CoreTestsExecutable = Join-Path $root 'build\tests\BrowserHistoryLauncher.CoreTests.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$CoreTestsExecutable = (Resolve-Path -LiteralPath $CoreTestsExecutable).Path

if (-not ('ListaryActionTestNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class ListaryActionTestNative {
    public const string ConfirmationTitle = "\u5f3a\u5236\u7ed3\u675f\u5e94\u7528";
    public const string InvalidActionTitle = "\u65e0\u6cd5\u6267\u884c Listary \u7ed3\u679c";
    private delegate bool EnumChildProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumChildProc callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("psapi.dll", SetLastError = true)]
    private static extern bool QueryWorkingSet(IntPtr process, IntPtr buffer, uint bytes);

    public static uint WindowProcessId(IntPtr window) {
        uint processId;
        GetWindowThreadProcessId(window, out processId);
        return processId;
    }
    public static IntPtr FindOwnedWindow(uint processId, string className, string title) {
        if (String.IsNullOrEmpty(className)) className = null;
        if (String.IsNullOrEmpty(title)) title = null;
        IntPtr window = IntPtr.Zero;
        while ((window = FindWindowEx(IntPtr.Zero, window, className, title)) != IntPtr.Zero) {
            if (WindowProcessId(window) == processId) return window;
        }
        return IntPtr.Zero;
    }
    public static bool AnyHostWindow() {
        return FindWindowEx(IntPtr.Zero, IntPtr.Zero, "BrowserHistoryLauncher.SearchWindow", null) != IntPtr.Zero;
    }
    public static IntPtr FindHostWindow() {
        return FindWindowEx(IntPtr.Zero, IntPtr.Zero, "BrowserHistoryLauncher.SearchWindow", null);
    }
    public static bool ShowOwnedHelperWindow(IntPtr window, uint processId) {
        var className = new StringBuilder(64);
        GetClassName(window, className, className.Capacity);
        if (WindowProcessId(window) != processId || className.ToString() != "BhlAppTerminationIsolatedTestWindow") return false;
        // Start-Process Hidden affects the first ShowWindow in the helper.
        // This second call makes only its offscreen test window enumerable.
        ShowWindow(window, 4); // SW_SHOWNOACTIVATE; keep its offscreen location.
        return IsWindowVisible(window);
    }
    public static string WindowText(IntPtr window) {
        var text = new StringBuilder(32768);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }
    public static string DialogText(IntPtr dialog) {
        var text = new StringBuilder();
        EnumChildWindows(dialog, delegate(IntPtr child, IntPtr ignored) {
            text.AppendLine(WindowText(child));
            return true;
        }, IntPtr.Zero);
        return text.ToString();
    }
    public static int DefaultButton(IntPtr dialog) {
        long result = SendMessage(dialog, 0x0400, UIntPtr.Zero, IntPtr.Zero).ToInt64();
        return ((result >> 16) & 0xffff) == 0x534b ? (int)(result & 0xffff) : 0;
    }
    public static bool ClickOwnedDialog(IntPtr dialog, uint processId, string title, int buttonId) {
        var className = new StringBuilder(64);
        GetClassName(dialog, className, className.Capacity);
        if (!IsWindow(dialog) || WindowProcessId(dialog) != processId ||
            className.ToString() != "#32770" || WindowText(dialog) != title) return false;
        IntPtr button = GetDlgItem(dialog, buttonId);
        // Ask the actual button to generate its normal click notification.
        // Posting successfully is not a pass: Wait-DialogClosed checks the outcome.
        return button != IntPtr.Zero && WindowProcessId(button) == processId &&
            PostMessage(button, 0x00F5, UIntPtr.Zero, IntPtr.Zero); // BM_CLICK
    }
    public static bool TryPrivateWorkingSet(IntPtr process, out ulong privateBytes) {
        privateBytes = 0;
        for (int bytes = 64 * 1024; bytes <= 8 * 1024 * 1024; bytes *= 2) {
            IntPtr buffer = Marshal.AllocHGlobal(bytes);
            try {
                if (!QueryWorkingSet(process, buffer, (uint)bytes)) {
                    if (Marshal.GetLastWin32Error() == 24) continue; // ERROR_BAD_LENGTH
                    return false;
                }
                ulong count = IntPtr.Size == 8 ? unchecked((ulong)Marshal.ReadInt64(buffer)) :
                    unchecked((uint)Marshal.ReadInt32(buffer));
                if (count > (ulong)(bytes / IntPtr.Size - 1)) return false;
                ulong privatePages = 0;
                for (ulong index = 0; index < count; ++index) {
                    int offset = checked((int)((index + 1) * (ulong)IntPtr.Size));
                    ulong flags = IntPtr.Size == 8 ? unchecked((ulong)Marshal.ReadInt64(buffer, offset)) :
                        unchecked((uint)Marshal.ReadInt32(buffer, offset));
                    // PSAPI_WORKING_SET_BLOCK: Protection 5 bits, ShareCount 3,
                    // then Shared at bit 8. Count resident nonshareable pages.
                    if ((flags & (1UL << 8)) == 0) ++privatePages;
                }
                privateBytes = checked(privatePages * (ulong)Environment.SystemPageSize);
                return true;
            } finally { Marshal.FreeHGlobal(buffer); }
        }
        return false;
    }
}
'@
}

function Assert-Check([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "FAIL $Name" }
    Write-Output "PASS $Name"
}

function Wait-OwnedWindow($Process, [string]$ClassName, [string]$Title) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $Process.Refresh()
        if ($Process.HasExited) { break }
        $window = [ListaryActionTestNative]::FindOwnedWindow($Process.Id, $ClassName, $Title)
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Expected test-owned window was not created: $ClassName / $Title"
}

function Wait-DialogClosed([IntPtr]$Window) {
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([ListaryActionTestNative]::IsWindow($Window) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 20
    }
    Assert-Check (-not [ListaryActionTestNative]::IsWindow($Window)) 'test action dialog closed'
}

function Test-Alive($Process) {
    $Process.Refresh()
    return -not $Process.HasExited
}

function Write-PrivateWorkingSetMetric($Process, [string]$Stage) {
    $source = 'Win32_PerfFormattedData_PerfProc_Process.WorkingSetPrivate'
    try {
        $samples = @(Get-CimInstance -ClassName Win32_PerfFormattedData_PerfProc_Process -Filter (
            'IDProcess = ' + $Process.Id) -OperationTimeoutSec 2 -ErrorAction Stop)
        if ($samples.Count -eq 1 -and $null -ne $samples[0].WorkingSetPrivate) {
            Write-Output "METRIC end_$Stage private_working_set_bytes=$($samples[0].WorkingSetPrivate) source=$source"
            return
        }
    } catch { }
    try {
        [uint64]$privateBytes = 0
        if ([ListaryActionTestNative]::TryPrivateWorkingSet($Process.Handle, [ref]$privateBytes)) {
            Write-Output "METRIC end_$Stage private_working_set_bytes=$privateBytes source=QueryWorkingSet_nonshared_pages fallback_from=$source"
            return
        }
    } catch { }
    Write-Output "METRIC end_$Stage private_working_set_bytes=unavailable source=WMI_and_QueryWorkingSet"
}

function Get-EndSuggestions([string]$Query) {
    $response = Invoke-RestMethod -Uri ('http://127.0.0.1:32119/suggest?prefix=end&q=' +
        [Uri]::EscapeDataString($Query)) -TimeoutSec 5
    if ($response.Count -ne 2 -or $response[0] -ne $Query) { throw 'Unexpected end suggestion response.' }
    return ,@($response[1])
}

function Send-EndSelection([string]$Selection) {
    # Explicit executable launch avoids changing the user's registered bhl protocol.
    $uri = 'bhl://end?prefix=end&selection=' + [Uri]::EscapeDataString($Selection)
    $forwarder = Start-Process -FilePath $Executable -ArgumentList @('--listary-open', ('"' + $uri + '"')) -PassThru -WindowStyle Hidden
    $script:createdForwarders.Add($forwarder)
    if (-not $forwarder.WaitForExit(5000) -or $forwarder.ExitCode -ne 0) {
        throw 'The explicit --listary-open invocation did not forward successfully.'
    }
}

function Confirm-TargetPrompt([IntPtr]$Dialog, $TargetProcess, [string]$TargetTitle, [string]$TargetPath) {
    $text = [ListaryActionTestNative]::DialogText($Dialog)
    $pidPattern = 'PID\D*' + [regex]::Escape([string]$TargetProcess.Id) + '(?:\D|$)'
    Assert-Check ($text.Contains($TargetTitle) -and $text.Contains($TargetPath) -and $text -match $pidPattern) 'confirmation identifies the exact test helper title, path, and PID'
    Assert-Check ([ListaryActionTestNative]::DefaultButton($Dialog) -eq 7) 'force-termination confirmation defaults to No'
}

function Stop-CreatedProcess($Process, [string]$ClassName, [uint32]$CloseMessage) {
    if (-not $Process) { return }
    try {
        if (Test-Alive $Process) {
            $window = [ListaryActionTestNative]::FindOwnedWindow($Process.Id, $ClassName, $null)
            if ($window -ne [IntPtr]::Zero) {
                [ListaryActionTestNative]::PostMessage($window, $CloseMessage, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            }
            if (-not $Process.WaitForExit(2000)) {
                # Use the retained handle from our own Start-Process, never a name-wide stop.
                [ListaryActionTestNative]::TerminateProcess($Process.Handle, 123) | Out-Null
                if (-not $Process.WaitForExit(3000)) { Write-Warning "Test-created PID $($Process.Id) did not exit." }
            }
        }
    } finally { $Process.Dispose() }
}

if ((Get-Process -Name BrowserHistoryLauncher -ErrorAction SilentlyContinue) -or
    [ListaryActionTestNative]::AnyHostWindow()) {
    throw 'A launcher is already running. Exit it before this isolated action test.'
}
$probe = [Net.Sockets.TcpClient]::new()
try {
    try { $probe.Connect('127.0.0.1', 32119) } catch [Net.Sockets.SocketException] { }
    if ($probe.Connected) { throw 'Port 32119 is already in use; no existing service will be targeted.' }
} finally { $probe.Dispose() }

$hostProcess = $null
$first = $null
$second = $null
$hostWindow = [IntPtr]::Zero
$script:createdForwarders = [Collections.Generic.List[Diagnostics.Process]]::new()
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$temporaryDirectory = Join-Path $temporaryRoot ('BhlActions-' + [Guid]::NewGuid().ToString('N'))
$helperExecutable = Join-Path $temporaryDirectory 'AppActionTarget.exe'
$temporaryCreated = $false
$testStartedUtc = [DateTime]::UtcNow
try {
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    $temporaryCreated = $true
    Copy-Item -LiteralPath $CoreTestsExecutable -Destination $helperExecutable
    $hostProcess = Start-Process -FilePath $Executable -PassThru -WindowStyle Hidden
    $hostWindow = Wait-OwnedWindow $hostProcess 'BrowserHistoryLauncher.SearchWindow' $null
    $health = $null
    for ($attempt = 0; $attempt -lt 50 -and -not $health; $attempt++) {
        try { $health = Invoke-RestMethod -Uri 'http://127.0.0.1:32119/health' -TimeoutSec 1 }
        catch { Start-Sleep -Milliseconds 100 }
    }
    Assert-Check ($health.ok -and (Test-Alive $hostProcess)) 'test-created host is healthy'
    $hostProcess.Refresh()
    Write-Output "METRIC end_before_working_set_bytes=$($hostProcess.WorkingSet64) private_bytes=$($hostProcess.PrivateMemorySize64)"
    Write-PrivateWorkingSetMetric $hostProcess 'before_query'

    $commonTitle = 'BHL End E2E ' + [Guid]::NewGuid().ToString('N')
    $firstTitle = $commonTitle + ' first'
    $secondTitle = $commonTitle + ' second'
    $first = Start-Process -FilePath $helperExecutable -ArgumentList @('--app-termination-test-helper', ('"' + $firstTitle + '"')) -PassThru -WindowStyle Hidden
    $second = Start-Process -FilePath $helperExecutable -ArgumentList @('--app-termination-test-helper', ('"' + $secondTitle + '"')) -PassThru -WindowStyle Hidden
    $firstWindow = Wait-OwnedWindow $first 'BhlAppTerminationIsolatedTestWindow' $firstTitle
    $secondWindow = Wait-OwnedWindow $second 'BhlAppTerminationIsolatedTestWindow' $secondTitle
    Assert-Check ([ListaryActionTestNative]::ShowOwnedHelperWindow($firstWindow, $first.Id) -and
        [ListaryActionTestNative]::ShowOwnedHelperWindow($secondWindow, $second.Id)) 'own offscreen helper windows are visible to enumeration without activation'
    Assert-Check ((Test-Alive $first) -and (Test-Alive $second)) 'two independent same-executable test helpers are alive'

    $queryWatch = [Diagnostics.Stopwatch]::StartNew()
    $rows = Get-EndSuggestions $commonTitle
    $queryWatch.Stop()
    Write-Output "METRIC end_query_elapsed_ms=$($queryWatch.Elapsed.TotalMilliseconds.ToString('F3', [Globalization.CultureInfo]::InvariantCulture))"
    Write-PrivateWorkingSetMetric $hostProcess 'after_query'
    Assert-Check ($rows.Count -eq 2) 'HTTP end query returns exactly the two unique test windows'
    $firstRows = @($rows | Where-Object { $_.Contains($firstTitle) -and $_.EndsWith('PID ' + $first.Id) })
    $secondRows = @($rows | Where-Object { $_.Contains($secondTitle) -and $_.EndsWith('PID ' + $second.Id) })
    Assert-Check ($firstRows.Count -eq 1 -and $secondRows.Count -eq 1) 'HTTP rows identify the exact independent helper PIDs'

    Send-EndSelection $commonTitle
    $dialog = Wait-OwnedWindow $hostProcess '#32770' ([ListaryActionTestNative]::InvalidActionTitle)
    Assert-Check ((Test-Alive $first) -and (Test-Alive $second)) 'raw query does not terminate or pick a helper'
    Assert-Check ([ListaryActionTestNative]::ClickOwnedDialog($dialog, $hostProcess.Id, [ListaryActionTestNative]::InvalidActionTitle, 1)) 'dismiss only the verified host invalid-selection dialog'
    Wait-DialogClosed $dialog

    $rows = Get-EndSuggestions $commonTitle
    $firstRows = @($rows | Where-Object { $_.Contains($firstTitle) -and $_.EndsWith('PID ' + $first.Id) })
    Assert-Check ($firstRows.Count -eq 1) 'first helper remains selectable after rejected raw input'
    Send-EndSelection $firstRows[0]
    $dialog = Wait-OwnedWindow $hostProcess '#32770' ([ListaryActionTestNative]::ConfirmationTitle)
    Confirm-TargetPrompt $dialog $first $firstTitle $helperExecutable
    Assert-Check ([ListaryActionTestNative]::ClickOwnedDialog($dialog, $hostProcess.Id, [ListaryActionTestNative]::ConfirmationTitle, 7)) 'cancel only the verified test-helper confirmation'
    Wait-DialogClosed $dialog
    Assert-Check ((Test-Alive $first) -and (Test-Alive $second)) 'No keeps both test helpers alive'
    Write-PrivateWorkingSetMetric $hostProcess 'after_cancel'

    $rows = Get-EndSuggestions $commonTitle
    $firstRows = @($rows | Where-Object { $_.Contains($firstTitle) -and $_.EndsWith('PID ' + $first.Id) })
    Assert-Check ($firstRows.Count -eq 1) 'cancelled helper can be selected again'
    Send-EndSelection $firstRows[0]
    $dialog = Wait-OwnedWindow $hostProcess '#32770' ([ListaryActionTestNative]::ConfirmationTitle)
    Confirm-TargetPrompt $dialog $first $firstTitle $helperExecutable
    Assert-Check ([ListaryActionTestNative]::ClickOwnedDialog($dialog, $hostProcess.Id, [ListaryActionTestNative]::ConfirmationTitle, 6)) 'confirm termination of only the verified first test helper'
    Assert-Check ($first.WaitForExit(5000)) 'selected helper actually exits after Yes'
    Assert-Check ($first.ExitCode -eq 1) 'selected helper exits through termination rather than its timeout'
    Wait-DialogClosed $dialog
    Assert-Check (Test-Alive $second) 'second instance of the same executable remains alive'
    $health = Invoke-RestMethod -Uri 'http://127.0.0.1:32119/health' -TimeoutSec 2
    Assert-Check ($health.ok -and (Test-Alive $hostProcess)) 'host remains healthy after end action'
    $remaining = Get-EndSuggestions $commonTitle
    Assert-Check ($remaining.Count -eq 1 -and $remaining[0].EndsWith('PID ' + $second.Id)) 'fresh end results contain only the surviving helper'
    $hostProcess.Refresh()
    Write-Output "METRIC end_after_working_set_bytes=$($hostProcess.WorkingSet64) private_bytes=$($hostProcess.PrivateMemorySize64) threads=$($hostProcess.Threads.Count) handles=$($hostProcess.HandleCount)"
    Write-PrivateWorkingSetMetric $hostProcess 'after_success'

    $previousHost = $hostProcess
    $previousHostId = $previousHost.Id
    Assert-Check ($previousHost.WaitForExit(12000)) 'inactive UI-loaded host exits for resource recycling within 12 seconds'
    $replacement = $null
    $replacementDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $candidateWindow = [ListaryActionTestNative]::FindHostWindow()
        if ($candidateWindow -ne [IntPtr]::Zero) {
            $candidateId = [ListaryActionTestNative]::WindowProcessId($candidateWindow)
            if ($candidateId -ne 0 -and $candidateId -ne $previousHostId) {
                $candidate = [Diagnostics.Process]::GetProcessById($candidateId)
                $accepted = $false
                try {
                    # Retain the handle before checking identity or doing later cleanup.
                    $null = $candidate.Handle
                    $candidatePath = [IO.Path]::GetFullPath($candidate.MainModule.FileName)
                    if (-not [string]::Equals($candidatePath, [IO.Path]::GetFullPath($Executable),
                            [StringComparison]::OrdinalIgnoreCase) -or
                        $candidate.StartTime.ToUniversalTime() -lt $testStartedUtc -or
                        -not $previousHost.HasExited -or $candidate.HasExited -or
                        [ListaryActionTestNative]::WindowProcessId($candidateWindow) -ne $candidate.Id) {
                        throw 'Replacement window did not identify a new instance of this test executable.'
                    }
                    $replacement = $candidate
                    $hostWindow = $candidateWindow
                    $accepted = $true
                } finally {
                    if (-not $accepted) { $candidate.Dispose() }
                }
                break
            }
        }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $replacementDeadline)
    Assert-Check ($null -ne $replacement) 'recycled host has a new PID, exact executable path, and test-owned start time'
    $hostProcess = $replacement
    $previousHost.Dispose()
    Write-Output "METRIC end_host_recycle previous_pid=$previousHostId recycled_pid=$($hostProcess.Id)"
    $health = $null
    for ($attempt = 0; $attempt -lt 25 -and -not $health; $attempt++) {
        try { $health = Invoke-RestMethod -Uri 'http://127.0.0.1:32119/health' -TimeoutSec 1 }
        catch { Start-Sleep -Milliseconds 100 }
    }
    Assert-Check ($health.ok -and (Test-Alive $hostProcess)) 'recycled test host is healthy'
    $hostProcess.Refresh()
    $cpuBeforeMs = $hostProcess.TotalProcessorTime.TotalMilliseconds
    $cpuWatch = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Milliseconds 1000
    $hostProcess.Refresh()
    $cpuAfterMs = $hostProcess.TotalProcessorTime.TotalMilliseconds
    $cpuWatch.Stop()
    $cpuPercent = 100.0 * ($cpuAfterMs - $cpuBeforeMs) / ($cpuWatch.Elapsed.TotalMilliseconds * [Environment]::ProcessorCount)
    Write-Output "METRIC end_idle_cpu_percent=$($cpuPercent.ToString('F4', [Globalization.CultureInfo]::InvariantCulture)) sample_ms=$($cpuWatch.ElapsedMilliseconds) logical_processors=$([Environment]::ProcessorCount) source=TotalProcessorTime"
    [uint64]$recycledPrivateWs = 0
    $measuredRecycledWs = [ListaryActionTestNative]::TryPrivateWorkingSet($hostProcess.Handle, [ref]$recycledPrivateWs)
    Assert-Check $measuredRecycledWs 'recycled host private working set is directly measurable'
    Write-Output "METRIC end_recycled private_working_set_bytes=$recycledPrivateWs source=QueryWorkingSet_nonshared_pages"
    Assert-Check ($recycledPrivateWs -gt 0 -and $recycledPrivateWs -le 15MB) 'recycled host returns to the 15 MiB private working set target'
    Write-Output 'SUMMARY listary_action_failures=0'
} finally {
    # These objects all came from this invocation. Do not touch existing user apps.
    foreach ($forwarder in $script:createdForwarders) { Stop-CreatedProcess $forwarder '' 0x0010 }
    Stop-CreatedProcess $first 'BhlAppTerminationIsolatedTestWindow' 0x0010
    Stop-CreatedProcess $second 'BhlAppTerminationIsolatedTestWindow' 0x0010
    Stop-CreatedProcess $hostProcess 'BrowserHistoryLauncher.SearchWindow' (0x8000 + 13)
    if ($temporaryCreated -and (Test-Path -LiteralPath $temporaryDirectory)) {
        $resolvedDirectory = (Resolve-Path -LiteralPath $temporaryDirectory).Path
        $resolvedParent = [IO.Path]::GetFullPath((Split-Path $resolvedDirectory -Parent)).TrimEnd('\')
        $leaf = Split-Path $resolvedDirectory -Leaf
        $attributes = (Get-Item -LiteralPath $resolvedDirectory).Attributes
        if ($resolvedParent -ne $temporaryRoot -or $leaf -notmatch '^BhlActions-[0-9a-f]{32}$' -or
            ($attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing to clean up an unexpected test directory.'
        }
        if (Test-Path -LiteralPath $helperExecutable) { Remove-Item -LiteralPath $helperExecutable -Force }
        # No recursive deletion: leave any unexpected contents for inspection.
        if (@(Get-ChildItem -LiteralPath $resolvedDirectory -Force).Count -ne 0) {
            throw "Unexpected contents remain in test directory: $resolvedDirectory"
        }
        Remove-Item -LiteralPath $resolvedDirectory -Force
    }
}
