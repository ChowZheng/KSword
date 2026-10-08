[CmdletBinding()]
param(
    [string]$CliPath = "$env:USERPROFILE\Desktop\Release\KswordCLI.exe",
    [string]$OutputPath = "$env:USERPROFILE\Desktop\ksword-cli-report-cases.json",
    [int]$TimeoutSeconds = 20,
    [switch]$EnsureDriverLoaded
)
$ErrorActionPreference = 'Stop'
$computer = Get-CimInstance Win32_ComputerSystem
if ($computer.Model -notmatch 'VMware') { throw 'This live regression runner requires a VMware guest.' }
$os = Get-CimInstance Win32_OperatingSystem
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$service = Get-CimInstance Win32_SystemDriver -Filter "Name='KswordARK'"
if ($EnsureDriverLoaded -and -not $service) {
    $driverPath = Join-Path (Split-Path $CliPath) 'KswordARK.sys'
    if (-not (Test-Path -LiteralPath $driverPath)) { throw "Guest driver missing: $driverPath" }
    $createOutput = (& sc.exe create KswordARK type= kernel start= demand binPath= $driverPath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "Could not create guest test service: $createOutput" }
    $service = Get-CimInstance Win32_SystemDriver -Filter "Name='KswordARK'"
}
$cases = @(
    'help', 'help driver', 'help handle', 'help process terminate',
    'help driver detail', 'help kernel object-summary',
    'log', 'log --max-frames 0', 'log --max-frames 1', 'log --max-frames 2', 'log --max-frames 100',
    'process enum --limit 4', 'thread enum --limit 4', 'handle enum --limit 4', "handle enum --pid $PID --limit 4",
    'kernel ssdt', 'kernel scan-inline-hooks --limit 4', 'kernel cid', 'kernel ipc', 'kernel object-summary --limit 4',
    'callback enum --limit 4', 'kernel callbacks --limit 2', 'callback runtime-state', 'callback monitor-status',
    'driver integrity --limit 4', 'driver unloaded --limit 4', 'driver piddb --limit 4',
    'driver device --limit 4', 'driver major --limit 4', 'driver fastio --limit 4',
    'r0 object-types --limit 4', 'r0 object-types --max-entries 4', 'misc vbs', 'dyn status', 'capability query-driver-capabilities',
    'preflight query --limit 32', 'r0 ioctl-registry --max-entries 512',
    'driver detail --name SkaProtect', 'driver detail', 'kernel object-summary', 'process terminate', 'process frobnicate'
)
$results = [Collections.Generic.List[object]]::new()
$started = $false
try {
    if ($service -and $service.State -ne 'Running') {
        $startOutput = (& sc.exe start KswordARK 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) { throw "Could not start existing guest driver: $startOutput" }
        $started = $true
    }
    foreach ($command in $cases) {
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = (Resolve-Path $CliPath).Path
        $info.WorkingDirectory = Split-Path $info.FileName
        $info.Arguments = $command
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.StandardOutputEncoding = [Text.Encoding]::UTF8
        $info.StandardErrorEncoding = [Text.Encoding]::UTF8
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $info
        $watch = [Diagnostics.Stopwatch]::StartNew()
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timeout = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timeout) {
            $process.Kill()
            if (-not $process.WaitForExit(5000)) { throw "Timed-out CLI did not exit: $command" }
        }
        $process.WaitForExit()
        $results.Add([pscustomobject]@{
            command = $command; exitCode = $process.ExitCode; timeout = $timeout
            milliseconds = $watch.ElapsedMilliseconds
            stdout = $stdout.GetAwaiter().GetResult(); stderr = $stderr.GetAwaiter().GetResult()
        })
        $process.Dispose()
    }
} finally {
    if ($started -and -not $EnsureDriverLoaded) { $stopOutput = (& sc.exe stop KswordARK 2>&1 | Out-String) }
    $directory = Split-Path $CliPath
    $files = @(Get-ChildItem $directory -File | Where-Object { $_.Name -in @('KswordCLI.exe','KswordCLI.fixed.exe','KswordARK.sys') } | ForEach-Object {
        [pscustomobject]@{ name=$_.Name; length=$_.Length; sha256=(Get-FileHash $_.FullName).Hash; lastWriteTime=$_.LastWriteTimeUtc.ToString('o') }
    })
    [pscustomobject]@{
        os=$os.Caption; build=$os.BuildNumber; version=$os.Version; admin=$admin
        cliPath=$CliPath; files=$files
        service=if ($service) { [pscustomobject]@{state=$service.State; path=$service.PathName} } else { $null }
        finalService=Get-CimInstance Win32_SystemDriver -Filter "Name='KswordARK'" | Select-Object State,PathName
        startedExistingService=$started; results=@($results.ToArray())
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
}
