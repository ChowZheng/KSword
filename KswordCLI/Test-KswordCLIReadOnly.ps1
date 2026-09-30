<#
.SYNOPSIS
    Runs bounded, live read-only KswordCLI checks next to KswordCLI.exe and KswordARK.sys.

.DESCRIPTION
    Run elevated in the VM. The script replaces any existing KswordARK service with
    the sibling driver, exercises every command in its read-only catalog, writes a
    per-command transcript under TestResults-KswordCLI-<timestamp>, then always
    stops and removes the KswordARK service (including a service that existed before
    this run). Drain commands are included because they are read operations; capture
    start/stop and all data/control mutations are excluded.
#>
[CmdletBinding()]
param(
    [ValidateRange(5, 600)]
    [int]$CommandTimeoutSeconds = 60,

    [ValidateRange(5, 120)]
    [int]$WaitEventTimeoutSeconds = 15
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:ServiceName = 'KswordARK'
$script:CliPath = Join-Path $PSScriptRoot 'KswordCLI.exe'
$script:DriverPath = Join-Path $PSScriptRoot 'KswordARK.sys'
$script:RunId = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:ResultDirectory = Join-Path $PSScriptRoot "TestResults-KswordCLI-$($script:RunId)"
$script:Cases = [System.Collections.Generic.List[object]]::new()
$script:Results = [System.Collections.Generic.List[object]]::new()
$script:DriverSetupStarted = $false
$script:DriverStarted = $false
$script:CleanupStatus = 'NOT_STARTED'
$script:CleanupError = ''
$script:FixtureAddress = [IntPtr]::Zero
$script:FixtureBytes = [byte[]]@(
    0x4B, 0x53, 0x57, 0x4F, 0x52, 0x44, 0x43, 0x4C,
    0x49, 0x52, 0x45, 0x41, 0x44, 0x31, 0x32, 0x33)

function Write-Status {
    param([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::Gray)
    Write-Host "[KswordCLI-readonly] $Message" -ForegroundColor $Color
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Sc {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $text = (& "$env:SystemRoot\System32\sc.exe" @Arguments 2>&1 | Out-String).Trim()
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Text = $text }
}

function Wait-ServiceState {
    param(
        [Parameter(Mandatory)][ValidateSet('RUNNING', 'STOPPED', 'MISSING')][string]$State,
        [int]$TimeoutSeconds = 15
    )
    $until = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $q = Invoke-Sc -Arguments @('query', $script:ServiceName)
        if ($State -eq 'MISSING' -and $q.ExitCode -eq 1060) { return $true }
        if ($q.ExitCode -ne 0 -and $q.ExitCode -ne 1060) {
            throw "sc.exe query failed while waiting for $State (exit=$($q.ExitCode)): $($q.Text)"
        }
        if ($q.Text -match "(?im)\b$State\b") { return $true }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    return $false
}

function Remove-DriverService {
    param([switch]$Quiet)
    $query = Invoke-Sc -Arguments @('query', $script:ServiceName)
    if ($query.ExitCode -eq 1060) { return }
    if ($query.ExitCode -ne 0) { throw "Could not query $($script:ServiceName): $($query.Text)" }

    if (-not $Quiet) { Write-Status 'Stopping and deleting KswordARK service.' }
    $stop = Invoke-Sc -Arguments @('stop', $script:ServiceName)
    # A service can already be stopped. Wait for the state rather than relying on
    # sc.exe's localized text or exit code for that harmless case.
    if (-not (Wait-ServiceState -State 'STOPPED' -TimeoutSeconds 20)) {
        throw "Could not stop $($script:ServiceName): $($stop.Text)"
    }
    $delete = Invoke-Sc -Arguments @('delete', $script:ServiceName)
    if ($delete.ExitCode -ne 0 -and -not (Wait-ServiceState -State 'MISSING' -TimeoutSeconds 3)) {
        throw "Could not delete $($script:ServiceName): $($delete.Text)"
    }
    if (-not (Wait-ServiceState -State 'MISSING' -TimeoutSeconds 10)) {
        throw "Service $($script:ServiceName) is still registered after deletion."
    }
}

function Start-LocalDriver {
    # Even a preflight failure must honour the requested unconditional cleanup of
    # a previously loaded service.
    $script:DriverSetupStarted = $true
    if (-not (Test-Administrator)) { throw 'Run this script from an elevated PowerShell session.' }
    if (-not (Test-Path -LiteralPath $script:CliPath -PathType Leaf)) {
        throw "KswordCLI.exe is not beside this script: $script:CliPath"
    }
    if (-not (Test-Path -LiteralPath $script:DriverPath -PathType Leaf)) {
        throw "KswordARK.sys is not beside this script: $script:DriverPath"
    }

    New-Item -ItemType Directory -Path $script:ResultDirectory -Force | Out-Null
    Write-Status "Loading sibling driver: $($script:DriverPath)" Cyan

    # The requested VM test owns the complete service lifecycle. Recreate the
    # service so the tested CLI is paired with this directory's driver binary.
    Remove-DriverService -Quiet
    $create = Invoke-Sc -Arguments @(
        'create', $script:ServiceName, 'type=', 'kernel', 'start=', 'demand',
        'binPath=', $script:DriverPath)
    if ($create.ExitCode -ne 0) { throw "sc.exe create failed: $($create.Text)" }

    $start = Invoke-Sc -Arguments @('start', $script:ServiceName)
    if ($start.ExitCode -ne 0) {
        throw "sc.exe start failed: $($start.Text). Driver signing or test-signing policy may be blocking the load."
    }
    if (-not (Wait-ServiceState -State 'RUNNING' -TimeoutSeconds 20)) {
        throw "KswordARK service did not reach RUNNING: $($start.Text)"
    }
    $script:DriverStarted = $true
    Write-Status 'Driver is RUNNING.' Green
}

function Quote-WindowsArgument {
    param([AllowEmptyString()][string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]92) { $slashes++; continue }
        if ($character -eq '"') {
            [void]$builder.Append(([string][char]92 * (($slashes * 2) + 1)))
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) { [void]$builder.Append(([string][char]92 * $slashes)); $slashes = 0 }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) { [void]$builder.Append(([string][char]92 * ($slashes * 2))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-NativeCli {
    param(
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][string]$OutputPrefix,
        [Parameter(Mandatory)][int]$TimeoutSeconds
    )
    $stdoutPath = "$OutputPrefix.stdout.txt"
    $stderrPath = "$OutputPrefix.stderr.txt"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $script:CliPath
    $startInfo.WorkingDirectory = $PSScriptRoot
    $startInfo.Arguments = (($Arguments | ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $clock = [Diagnostics.Stopwatch]::StartNew()
    if (-not $process.Start()) { throw "Could not start $($script:CliPath)" }
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    $exited = $process.WaitForExit($TimeoutSeconds * 1000)
    $terminatedAfterTimeout = $true
    if (-not $exited) {
        try { $process.Kill() } catch { }
        $terminatedAfterTimeout = $process.WaitForExit(5000)
    }
    if ($exited -or $terminatedAfterTimeout) {
        $process.WaitForExit()
        [void]$outTask.Wait(2000)
        [void]$errTask.Wait(2000)
    }
    $clock.Stop()
    $stdout = if ($outTask.IsCompleted) { $outTask.Result } else { '' }
    $stderr = if ($errTask.IsCompleted) { $errTask.Result } else { '' }
    [IO.File]::WriteAllText($stdoutPath, $stdout, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($stderrPath, $stderr, [Text.UTF8Encoding]::new($false))
    $exitCode = if ($exited) { $process.ExitCode } else { $null }
    $process.Dispose()
    return [pscustomobject]@{
        ExitCode = $exitCode
        TimedOut = -not $exited
        TerminatedAfterTimeout = $terminatedAfterTimeout
        DurationMs = [long]$clock.ElapsedMilliseconds
        Stdout = $stdout
        Stderr = $stderr
        StdoutFile = $stdoutPath
        StderrFile = $stderrPath
    }
}

function Get-HelpCommandKeys {
    $helpRun = Invoke-NativeCli -Arguments @('help') `
        -OutputPrefix (Join-Path $script:ResultDirectory 'help-inventory') -TimeoutSeconds 15
    if ($helpRun.TimedOut -or $helpRun.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($helpRun.Stdout)) {
        throw "Could not read CLI command inventory. Exit=$($helpRun.ExitCode), stderr=$($helpRun.Stderr)"
    }
    $keys = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in ($helpRun.Stdout -split "`r?`n")) {
        if ($line -match '^\s*(?<family>[a-z][a-z0-9-]*)\s+(?<subcommands>.+?)\s*$') {
            $family = $Matches.family.ToLowerInvariant()
            $subcommands = $Matches.subcommands.Trim()
            if ($family -eq 'log') { [void]$keys.Add('log'); continue }
            if ($subcommands -match '^(\[no subcommand\]|\[no subcommands\])$') { continue }
            foreach ($subcommand in ($subcommands -split '\s*\|\s*')) {
                if ($subcommand -match '^[a-z][a-z0-9-]*$') { [void]$keys.Add("$family $subcommand") }
            }
        }
    }
    return ,$keys
}

function Get-ExcludedCommandKeys {
    # These are the documented state-changing commands, plus ddma read (which
    # temporarily overwrites and restores scratch disk sectors).
    return @(
        'process terminate', 'process suspend', 'process resume', 'process set-ppl',
        'process set-integrity', 'process inject-dll', 'process inject-shellcode',
        'process set-visibility', 'process set-special-flags', 'process dkom',
        'memory write-va', 'memory write-phys', 'file delete-path', 'file set-integrity',
        'file monitor-control', 'kernel patch-inline-hook', 'kernel force-unload-driver',
        'callback set-rules', 'callback monitor-start', 'callback monitor-stop',
        'callback answer-event', 'callback cancel-pending', 'callback remove',
        'callback remove-ex', 'callback set-minifilter-bypass-pids',
        'dyn apply-profile-v4', 'dyn apply-profile', 'dyn apply-profile-ex', 'ddma read',
        'safety set-policy', 'registry set-value', 'registry delete-value',
        'registry create-key', 'registry delete-key', 'registry rename-value',
        'registry rename-key', 'redirect set-rules', 'network set-rules',
        'hwid dispatch-control', 'mutation prepare', 'mutation commit', 'mutation rollback'
    )
}

function New-Case {
    param([string]$Line)
    $tokens = @([regex]::Matches($Line.Trim(), '"[^"]*"|[^\s]+') | ForEach-Object {
        $token = $_.Value
        if ($token.Length -ge 2 -and $token[0] -eq '"' -and $token[$token.Length - 1] -eq '"') {
            $token.Substring(1, $token.Length - 2)
        } else { $token }
    })
    if ($tokens.Count -lt 1) { return }
    $key = if ($tokens[0] -eq 'log') { 'log' } else { "$($tokens[0]) $($tokens[1])" }
    $script:Cases.Add([pscustomobject]@{ Key = $key; Arguments = [string[]]$tokens })
}

function Build-ReadOnlyCatalog {
    # This allowlist is intentionally explicit. It is compared with `help` at run
    # time so a stale or mismatched CLI cannot silently appear fully tested.
    $specifications = @'
log --max-frames 8
process enum --limit 4
process crossview --max-nodes 16 --limit 4
process detail --pid {PID}
process runtime-fields --pid {PID} --items 1:0:8,2:8:8 --limit 4
memory query-va --pid {PID} --address {BUFFER}
memory read-va --pid {PID} --address {BUFFER} --bytes 16 --hexdump
memory translate-va --pid {PID} --address {BUFFER}
memory read-phys --address {PA} --bytes 16 --hexdump
memory query-pte --pid {PID} --address {BUFFER}
memory scan-kexec --max-entries 8 --limit 4
memory enum-vad --pid {PID} --max-entries 8 --limit 4
memory scan-exec-pte --pid {PID} --max-entries 8 --max-table-reads 64 --limit 4
memory read-section-pages --pid {PID} --start {IMAGE} --end {IMAGEEND} --max-pages 1 --flags 0x1 --limit 2
memory scan-evidence --max-rows 8 --max-bytes 4096 --max-bigpool-rows 8 --sample-bytes 16 --limit 4
file query-info --path {FILE}
file fileobject --path {FILE}
file section
file minifilter --max-rows 8 --limit 4
file bitlocker --max-rows 8 --max-depth 4 --volume {VOLUME} --limit 4
file storage --max-rows 8 --max-depth 4 --volume {VOLUME} --limit 4
file mountmgr --max-rows 8 --max-depth 4 --volume {VOLUME} --limit 4
file filesystem --max-rows 8 --max-depth 4 --volume {VOLUME} --limit 4
file monitor-drain --max-events 4
file monitor-status
kernel ssdt --limit 4
kernel shadow-ssdt --limit 4
kernel scan-inline-hooks --max-entries 8 --module ntoskrnl.exe --limit 4
kernel enum-iat-eat-hooks --max-entries 8 --module ntoskrnl.exe --limit 4
kernel query-driver-object --driver KswordARK --max-devices 8 --max-attached 8 --limit 4
kernel query-driver-integrity --driver KswordARK --max-rows 8 --max-idt-vectors 8 --max-devices 8 --max-attached 8 --limit 4
kernel query-cpu
kernel query-phys-layout
kernel cid --max-entries 8 --max-visits 64 --limit 4
kernel object-summary --target-kind 1 --cid {PID}
kernel ipc --pid {PID} --max-entries 8
kernel callbacks --max-entries 8 --limit 4
kernel hooks --max-entries 8 --module ntoskrnl.exe --limit 4
callback runtime-state
callback monitor-status
callback monitor-read --max-records 4 --limit 4
callback wait-event --waiter-tag 1397969713
callback query-minifilter-bypass-pids
callback enum --max-entries 8 --limit 4
dyn status
dyn fields --limit 4
dyn capabilities
dyn profile --limit 4
dyn v4-modules --limit 4
dyn v4-capabilities --limit 4
dyn capability-groups --limit 4
dyn v4-missing --limit 4
dyn missing-items --limit 4
dyn v4-items --limit 4
thread enum --pid {PID} --limit 4
thread crossview --pid {PID} --max-nodes 16 --limit 4
thread detail --tid {TID} --pid {PID}
thread runtime-fields --tid {TID} --pid {PID} --items 1:0:8 --limit 4
handle enum --pid {PID} --limit 16
handle object-table --pid {PID} --limit 16
handle query-object --pid {PID} --handle {HANDLE}
handle object-header --pid {PID} --handle {HANDLE}
handle type-matrix --pid {PID} --handle {HANDLE}
driver integrity --driver KswordARK --max-rows 8 --max-idt-vectors 8 --max-devices 8 --max-attached 8 --limit 4
driver detail --driver KswordARK --max-devices 8 --max-attached 8 --limit 4
driver device --max-rows 8 --max-attached 8 --limit 4
driver major --max-rows 8 --max-attached 8 --limit 4
driver fastio --max-rows 8 --max-attached 8 --limit 4
driver unloaded --max-rows 8 --max-idt-vectors 8 --max-devices 8 --max-attached 8 --limit 4
driver piddb --max-rows 8 --max-idt-vectors 8 --max-devices 8 --max-attached 8 --limit 4
hardware audit --max-rows 8 --max-attached 8 --limit 4
hardware pnp --max-rows 8 --max-attached 8 --limit 4
hardware input --max-rows 8 --max-attached 8 --limit 4
hardware usb --max-rows 8 --max-attached 8 --limit 4
hwid dispatch-query
window win32k --pid {WINDOWPID} --tid {WINDOWTID} --max-entries 8 --limit 4
window gui --pid {WINDOWPID} --tid {WINDOWTID} --max-entries 8 --limit 4
window gui-threads --pid {WINDOWPID} --tid {WINDOWTID} --max-entries 8 --limit 4
window hotkeys-pdb --pid {WINDOWPID} --tid {WINDOWTID} --max-entries 8 --limit 4
window hooks-pdb --pid {WINDOWPID} --tid {WINDOWTID} --max-entries 8 --limit 4
window detail --hwnd {HWND} --pid {WINDOWPID} --tid {WINDOWTID}
window gpu --max-rows 8 --max-attached 8 --limit 4
window display --max-rows 8 --max-attached 8 --limit 4
window watchdog --max-rows 8 --max-attached 8 --limit 4
misc security
misc ci
misc vbs
misc hyperv
misc applocker
misc bam
misc driver-trust --max-entries 8 --limit 4
alpc query-port --pid {PID} --handle {HANDLE}
section query-process --pid {PID} --max-mappings 8 --limit 4
section query-file-mappings --path {FILE_NT} --max-mappings 8 --limit 4
trust query-image --path {FILE}
safety query-policy
preflight query --limit 8
registry read-value --key "\REGISTRY\MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion" --value ProductName --max-data-bytes 128
registry enum-key --key "\REGISTRY\MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion" --max-subkeys 8 --max-values 8 --max-value-data-bytes 32 --limit 4
redirect query-status --limit 4
network query-status --limit 4
network audit --max-rows 8 --limit 4
network tcp --max-rows 8 --limit 4
network udp --max-rows 8 --limit 4
network wfp --max-rows 8 --limit 4
network ndis --max-rows 8 --limit 4
network afd --limit 4
network nsi --limit 4
keyboard enum-hotkeys --pid {PID} --max-entries 8 --limit 4
keyboard enum-hooks --pid {PID} --max-entries 8 --limit 4
mutation query-audit --max-entries 8 --limit 4
capability query-driver-capabilities --limit 8
wsl query-silo --pid {PID} --tid {TID}
ddma selftest
ddma probe --max-disks 2
r0 workqueue --max-entries 8 --limit 4
r0 directory --path {SYSTEMDIR} --max-entries 8 --limit 4
r0 directory-irp --path {SYSTEMDIR} --max-entries 8 --limit 4
r0 image-signature --path {FILE}
r0 debug-output --max-records 4 --limit 4
r0 hvm-status
r0 hvm-metrics
r0 hvm-events --max-rows 4
r0 hvm-platform
r0 ioctl-registry --max-entries 8
r0 timer-dpc --max-entries 8 --max-per-bucket 4
r0 unloaded --source mm --max-rows 4
r0 wfp-events --max-rows 4
r0 traffic --max-rows 4
r0 piddb --max-rows 4
r0 cpu-power
r0 process-protect
r0 raw-disk-backend --disk 0
r0 raw-disk-read --disk 0 --length 16 --offset 0 --hexdump
r0 system-time
r0 slat-iommu
r0 platform --max-rows 4
r0 i8042 --max-rows 4
r0 object-types --max-entries 8
r0 object-type-procedures --max-entries 8
r0 win32k-timers --pid {WINDOWPID} --tid {WINDOWTID} --max-entries 4
r0 win32k-events --pid {WINDOWPID} --tid {WINDOWTID} --max-entries 4
'@

    foreach ($line in ($specifications -split "`r?`n")) {
        if (-not [string]::IsNullOrWhiteSpace($line)) { New-Case -Line $line }
    }
}

function Get-TestEnvironment {
    $systemRoot = $env:SystemRoot
    $systemDir = Join-Path $systemRoot 'System32'
    $systemFile = Join-Path $systemDir 'kernel32.dll'
    $volume = [IO.Path]::GetPathRoot($systemRoot)
    $process = Get-Process -Id $PID
    $module = $process.Modules | Where-Object { $_.ModuleName -ieq 'powershell.exe' -or $_.ModuleName -ieq 'pwsh.exe' } | Select-Object -First 1
    if ($null -eq $module) { $module = $process.MainModule }
    $imageBase = [long]$module.BaseAddress.ToInt64()
    $pageSize = [Environment]::SystemPageSize
    $imageStart = [long]([Math]::Floor($imageBase / $pageSize) * $pageSize)
    $consoleSig = @'
using System;
using System.Runtime.InteropServices;
public static class KswordCliTestNative {
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int handle);
    [DllImport("user32.dll", SetLastError=true)] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
}
'@
    if (-not ('KswordCliTestNative' -as [type])) { Add-Type -TypeDefinition $consoleSig -Language CSharp | Out-Null }
    $threadId = [KswordCliTestNative]::GetCurrentThreadId()
    $hwnd = [KswordCliTestNative]::GetConsoleWindow().ToInt64()
    if ($hwnd -eq 0) { throw 'This test requires a console window for the window detail query.' }
    [uint32]$windowProcessId = 0
    [uint32]$windowThreadId = [KswordCliTestNative]::GetWindowThreadProcessId([IntPtr]$hwnd, [ref]$windowProcessId)
    $stdInput = [KswordCliTestNative]::GetStdHandle(-10).ToInt64()

    $memSig = @'
using System;
using System.Runtime.InteropServices;
public static class KswordCliReadFixture {
    private const uint MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, PAGE_READWRITE = 0x04;
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint allocationType, uint protect);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool VirtualLock(IntPtr address, UIntPtr size);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool VirtualUnlock(IntPtr address, UIntPtr size);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool VirtualFree(IntPtr address, UIntPtr size, uint freeType);
    public static IntPtr Allocate(byte[] bytes) {
        IntPtr p = VirtualAlloc(IntPtr.Zero, new UIntPtr(4096), MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if (p == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        if (!VirtualLock(p, new UIntPtr(4096))) { int e=Marshal.GetLastWin32Error(); VirtualFree(p, UIntPtr.Zero, 0x8000); throw new System.ComponentModel.Win32Exception(e); }
        Marshal.Copy(bytes, 0, p, bytes.Length);
        return p;
    }
    public static void Release(IntPtr p) {
        if (p == IntPtr.Zero) return;
        VirtualUnlock(p, new UIntPtr(4096));
        VirtualFree(p, UIntPtr.Zero, 0x8000);
    }
}
'@
    if (-not ('KswordCliReadFixture' -as [type])) { Add-Type -TypeDefinition $memSig -Language CSharp | Out-Null }
    $script:FixtureAddress = [KswordCliReadFixture]::Allocate($script:FixtureBytes)
    return @{
        PID = [string]$PID
        TID = [string]$threadId
        WINDOWPID = [string]$windowProcessId
        WINDOWTID = [string]$windowThreadId
        BUFFER = ('0x{0:X}' -f $script:FixtureAddress.ToInt64())
        PA = '0x1000'
        IMAGE = ('0x{0:X}' -f $imageStart)
        IMAGEEND = ('0x{0:X}' -f ($imageStart + $pageSize - 1))
        FILE = $systemFile
        FILE_NT = ("\??\$systemFile")
        SYSTEMDIR = $systemDir
        VOLUME = $volume
        HWND = ('0x{0:X}' -f $hwnd)
        HANDLE = if ($stdInput -gt 0) { '0x{0:X}' -f $stdInput } else { '0x0' }
    }
}

function Expand-CaseArguments {
    param([Parameter(Mandatory)][string[]]$Tokens, [Parameter(Mandatory)][hashtable]$Values)
    $expanded = [System.Collections.Generic.List[string]]::new()
    foreach ($token in $Tokens) {
        if ($token -match '^\{(?<key>[A-Z_]+)\}$') {
            $value = $Values[$Matches.key]
            if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
                throw "No safe runtime value is available for placeholder $($Matches.key)."
            }
            $expanded.Add([string]$value)
        } else {
            $expanded.Add($token)
        }
    }
    return ,([string[]]$expanded.ToArray())
}

function Get-Classification {
    param([string]$Key, [int]$ExitCode, [bool]$TimedOut, [string]$Output)
    if ($TimedOut) {
        if ($Key -eq 'callback wait-event') { return 'INCONCLUSIVE' }
        return 'FAIL'
    }
    if ($ExitCode -ne 0) {
        if ($Key -eq 'r0 object-type-procedures' -and $ExitCode -in 5, 6) { return 'PARTIAL' }
        if ($Output -match '(?i)unsupported|not supported|not applicable|STATUS_NOT_SUPPORTED|STATUS_NOT_IMPLEMENTED') { return 'UNSUPPORTED' }
        return 'FAIL'
    }
    if ($Output -match '(?i)\bunsupported\b|\bnot supported\b|\bnot applicable\b|\bpartial\b|\btruncated\b|\bbudget\b') {
        return 'UNSUPPORTED'
    }
    if ([string]::IsNullOrWhiteSpace($Output)) { return 'FAIL' }
    if ($Output -match '(?im)^\s*(usage:|help forms:|families and subcommands:|error: (unknown|.*requires|.*missing))') { return 'FAIL' }
    # A real dispatch prints a protocol response or an explicit R3/result summary.
    if ($Output -notmatch '(?i)(version\s*=|io_ok\s*=|status\s*=|count\s*=|rows\s*=|entries\s*=|bytesReturned\s*=|fallback|PASS|NOT_APPLICABLE|unsupported)') {
        return 'FAIL'
    }
    return 'PASS'
}

function Get-HandleFromOutput {
    param([string]$Text)
    $match = [regex]::Match($Text, '(?im)\bhandle(?:Value)?\s*=\s*(0x[0-9a-f]+|[0-9]+)')
    if ($match.Success) { return $match.Groups[1].Value }
    $match = [regex]::Match($Text, '(?im)^\s*\[[0-9]+\].*?\b(0x[0-9a-f]{1,8})\b')
    if ($match.Success) { return $match.Groups[1].Value }
    return $null
}

function Invoke-TestCase {
    param([Parameter(Mandatory)][object]$Case, [Parameter(Mandatory)][hashtable]$Environment)
    $safeName = $Case.Key -replace '[^a-zA-Z0-9_-]', '_'
    $caseDirectory = Join-Path $script:ResultDirectory $safeName
    New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
    $args = Expand-CaseArguments -Tokens $Case.Arguments -Values $Environment
    $timeout = if ($Case.Key -eq 'callback wait-event') { $WaitEventTimeoutSeconds } else { $CommandTimeoutSeconds }
    $result = Invoke-NativeCli -Arguments $args -OutputPrefix (Join-Path $caseDirectory 'cli') -TimeoutSeconds $timeout
    $combined = $result.Stdout + "`n" + $result.Stderr

    if ($Case.Key -eq 'memory translate-va' -and $result.ExitCode -eq 0 -and -not $result.TimedOut) {
        $physical = [regex]::Match($combined, '(?im)\bpa\s*=\s*(0x[0-9a-f]+)')
        if ($physical.Success) { $Environment.PA = $physical.Groups[1].Value }
    }

    if ($Case.Key -in @('memory read-va', 'memory read-phys') -and $result.ExitCode -eq 0 -and -not $result.TimedOut) {
        $expected = (($script:FixtureBytes | ForEach-Object { $_.ToString('x2') }) -join '\s+')
        if ($combined -notmatch "(?i)$expected") {
            $classification = 'FAIL'
            $detail = 'Read command returned success but the known 16-byte fixture was not present in its hexdump.'
        } else {
            $classification = 'PASS'
            $detail = 'Known 16-byte fixture matched.'
        }
    } elseif ($Case.Key -eq 'callback wait-event' -and $result.TimedOut -and $result.TerminatedAfterTimeout) {
        $classification = 'INCONCLUSIVE'
        $detail = "No callback event arrived within $WaitEventTimeoutSeconds seconds; wait path was entered and the child CLI was cancelled."
    } else {
        $classification = Get-Classification -Key $Case.Key -ExitCode $(if ($null -eq $result.ExitCode) { -1 } else { [int]$result.ExitCode }) -TimedOut $result.TimedOut -Output $combined
        $detail = if ($classification -eq 'UNSUPPORTED') { 'The command returned an explicit unsupported/partial result.' } else { '' }
    }

    $record = [ordered]@{
        command = ($args -join ' ')
        classification = $classification
        exitCode = $result.ExitCode
        timedOut = $result.TimedOut
        terminatedAfterTimeout = $result.TerminatedAfterTimeout
        durationMs = $result.DurationMs
        stdoutFile = [IO.Path]::GetFileName($result.StdoutFile)
        stderrFile = [IO.Path]::GetFileName($result.StderrFile)
        detail = $detail
    }
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $caseDirectory 'result.json') -Encoding UTF8
    $script:Results.Add([pscustomobject]$record)

    $color = switch ($classification) {
        'PASS' { [ConsoleColor]::Green }
        'UNSUPPORTED' { [ConsoleColor]::Yellow }
        'PARTIAL' { [ConsoleColor]::Yellow }
        'INCONCLUSIVE' { [ConsoleColor]::Yellow }
        default { [ConsoleColor]::Red }
    }
    Write-Status ("{0,-12} {1} (exit={2}, {3}ms)" -f $classification, $Case.Key, $result.ExitCode, $result.DurationMs) $color

    if ($Case.Key -eq 'handle enum' -and $classification -eq 'PASS' -and -not $Environment.HANDLE) {
        $Environment.HANDLE = Get-HandleFromOutput -Text $combined
        if ($Environment.HANDLE) { Write-Status "Using discovered current-process handle $($Environment.HANDLE) for handle/ALPC queries." }
    }
}

function Write-RunSummary {
    $counts = [ordered]@{}
    foreach ($classification in @('PASS', 'UNSUPPORTED', 'PARTIAL', 'INCONCLUSIVE', 'FAIL')) {
        $counts[$classification] = @($script:Results | Where-Object classification -eq $classification).Count
    }
    $summary = [ordered]@{
        runId = $script:RunId
        cliPath = [IO.Path]::GetFileName($script:CliPath)
        driverPath = [IO.Path]::GetFileName($script:DriverPath)
        testCount = $script:Cases.Count
        counts = $counts
        cleanup = $script:CleanupStatus
        cleanupError = $script:CleanupError
        overall = if ($counts.FAIL -gt 0 -or $counts.PASS -eq 0 -or $script:CleanupStatus -ne 'COMPLETE') { 'FAIL' } else { 'COMPLETE_WITH_ENVIRONMENTAL_LIMITS' }
        results = @($script:Results)
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $script:ResultDirectory 'summary.json') -Encoding UTF8
    $table = @($script:Results | Select-Object classification, command, exitCode, durationMs, detail)
    $table | Export-Csv -LiteralPath (Join-Path $script:ResultDirectory 'summary.csv') -NoTypeInformation -Encoding UTF8
    Write-Status "Summary: PASS=$($counts.PASS), UNSUPPORTED=$($counts.UNSUPPORTED), PARTIAL=$($counts.PARTIAL), INCONCLUSIVE=$($counts.INCONCLUSIVE), FAIL=$($counts.FAIL)" Cyan
    Write-Status "Reports: $($script:ResultDirectory)" Cyan
    return ($counts.FAIL -eq 0 -and $counts.PASS -gt 0 -and $script:CleanupStatus -eq 'COMPLETE')
}

$overallSuccess = $false
try {
    # Driver loading is the first external action after resolving the adjacent files.
    Start-LocalDriver
    Build-ReadOnlyCatalog

    $supported = Get-HelpCommandKeys
    $missing = @($script:Cases | Where-Object { -not $supported.Contains($_.Key) } | Select-Object -ExpandProperty Key -Unique)
    if ($missing.Count -gt 0) {
        throw "The sibling CLI is missing catalog commands: $($missing -join ', '). Use a CLI build matching this script."
    }
    $catalogKeys = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($case in $script:Cases) { [void]$catalogKeys.Add($case.Key) }
    $excludedKeys = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($key in (Get-ExcludedCommandKeys)) { [void]$excludedKeys.Add($key) }
    $unclassified = @($supported | Where-Object { -not $catalogKeys.Contains($_) -and -not $excludedKeys.Contains($_) })
    if ($unclassified.Count -gt 0) {
        throw "CLI help contains commands not classified as read-only or mutating: $($unclassified -join ', '). Update this script before testing this CLI build."
    }

    $environment = Get-TestEnvironment
    foreach ($case in $script:Cases) {
        try {
            Invoke-TestCase -Case $case -Environment $environment
        } catch {
            $script:Results.Add([pscustomobject][ordered]@{
                command = ($case.Arguments -join ' ')
                classification = 'FAIL'
                exitCode = $null
                timedOut = $false
                terminatedAfterTimeout = $true
                durationMs = 0
                stdoutFile = ''
                stderrFile = ''
                detail = $_.Exception.Message
            })
            Write-Status "FAIL         $($case.Key): $($_.Exception.Message)" Red
        }
    }
} catch {
    Write-Status "FATAL: $($_.Exception.Message)" Red
    if (Test-Path -LiteralPath $script:ResultDirectory) {
        [ordered]@{ runId = $script:RunId; overall = 'FATAL'; error = $_.Exception.Message } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $script:ResultDirectory 'fatal.json') -Encoding UTF8
    }
    $overallSuccess = $false
} finally {
    if ($script:FixtureAddress -ne [IntPtr]::Zero) {
        try { [KswordCliReadFixture]::Release($script:FixtureAddress) } catch { }
        $script:FixtureAddress = [IntPtr]::Zero
    }
    if ($script:DriverSetupStarted) {
        try {
            Remove-DriverService
            $script:CleanupStatus = 'COMPLETE'
        } catch {
            $script:CleanupStatus = 'FAILED'
            $script:CleanupError = $_.Exception.Message
            Write-Status "CLEANUP FAILURE: $($script:CleanupError)" Red
            $overallSuccess = $false
        }
    }
}

if ($script:Results.Count -gt 0) { $overallSuccess = Write-RunSummary }

if (-not $overallSuccess) { exit 1 }
exit 0
