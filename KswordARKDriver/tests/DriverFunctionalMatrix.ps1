<#
.SYNOPSIS
    在开启测试签名的专用 CI 机器上遍历 KswordARK 驱动功能，并把系统崩溃判为失败。

.DESCRIPTION
    本脚本按 tools/driver_functional_ci/driver_test_plan.json 逐条驱动 KswordCLI，
    每一步在执行前先把「正在执行哪个用例」写穿到磁盘日志（WriteThrough），
    所以即使机器当场 bugcheck，重启后也能准确知道崩溃发生在哪个用例上。

    崩溃判据取三路证据：新增的内核转储、System 日志里的 BugCheck(1001)/
    Kernel-Power(41)/EventLog(6008)，以及与基线不一致的 LastBootUpTime。

    「不是手贱导致的崩溃」由三层保证：
    1. 计划里任何命中危险模式的 IOCTL（写内核/物理内存、改 PatchGuard 覆盖区、
       DKOM 摘链、强卸驱动、改 HWID/时钟/电源、配置蓝屏路径等）都被静态门禁
       plan_gate.py 强制排到 excluded，CI 任何模式下都不会发出这些请求；
    2. probe 层的有界写操作全部带 targetGuard，运行时校验目标必须是 harness
       自己创建的进程/文件/注册表键，越界立即中止整轮，不允许打到系统对象上；
    3. guarded 层默认不执行；一旦显式打开，本次运行会被标记为
       attributionWeakened，崩溃归因为 harness-attributable 而不是驱动缺陷，
       并且该标记会保留到下次重启，覆盖 PatchGuard 延迟崩溃的时间窗。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File KswordARKDriver\tests\DriverFunctionalMatrix.ps1

.EXAMPLE
    # 上一轮崩溃已经人工定位完毕，解除阻塞后再跑
    ... -AcknowledgePreviousCrash
#>

[CmdletBinding()]
param(
    # RepositoryRoot：仓库根目录；留空时从脚本位置推导。
    [string]$RepositoryRoot,

    # PlanPath：功能矩阵计划文件。
    [string]$PlanPath,

    # BinaryDir：KswordARK.sys 与 KswordCLI.exe 所在目录。
    [string]$BinaryDir,

    # StateRoot：跨运行、跨重启保留的状态目录；必须在工作区之外。
    [string]$StateRoot,

    # ArtifactRoot：本次运行的日志与判定结果输出目录。
    [string]$ArtifactRoot,

    # ServiceName：被测驱动的内核服务名。
    [string]$ServiceName = 'KswordARK',

    # Mode：Run 执行完整矩阵；Verify 只做崩溃取证与判定；SelfTest 只自检 harness 自身逻辑。
    [ValidateSet('Run', 'Verify', 'SelfTest')]
    [string]$Mode = 'Run',

    # CaseFilter：只执行 id 匹配该正则的用例，用于定位单条回归。
    [string]$CaseFilter,

    # IncludeGuarded：执行 guarded 层用例；必须同时给出 -AcknowledgeGuardedRisk。
    [switch]$IncludeGuarded,

    # AcknowledgeGuardedRisk：确认承担 guarded 层的自伤风险与归因降级。
    [switch]$AcknowledgeGuardedRisk,

    # ConfigureCrashDump：允许脚本把机器改成保留内核转储；不给则只做校验。
    [switch]$ConfigureCrashDump,

    # AcknowledgePreviousCrash：上一轮崩溃已人工定位，解除对本次运行的阻塞。
    [switch]$AcknowledgePreviousCrash,

    # SkipDriverLoad：驱动已由 operator 自行加载，脚本不再创建/启动服务。
    [switch]$SkipDriverLoad,

    # FailOnSkippedProbe：probe 层用例因缺少变量被跳过时也判失败。
    [switch]$FailOnSkippedProbe
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# $PSScriptRoot 在部分宿主的 param 默认值求值阶段还是空的，改在脚本体里解析一次。
$script:ScriptDirectory = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }

$script:ExitPass = 0
$script:ExitCaseFailure = 1
$script:ExitCrash = 2
$script:ExitPreflight = 3

$script:Journal = $null
$script:SpawnedPids = New-Object System.Collections.Generic.HashSet[int]
$script:GuardedExecuted = $false

# ---------------------------------------------------------------------------
# 基础设施
# ---------------------------------------------------------------------------

function Write-Step {
    param([Parameter(Mandatory)][string]$Message)
    Write-Host "[driver-matrix] $Message"
}

function Get-UtcStamp {
    return (Get-Date).ToUniversalTime().ToString('o')
}

function Open-Journal {
    <#
      打开崩溃可存活的执行日志。输入是日志文件路径；处理过程用 WriteThrough
      打开 FileStream，使每条记录在返回前落盘；返回值是写入器句柄。
    #>
    param([Parameter(Mandatory)][string]$Path)

    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::Read,
        4096,
        [System.IO.FileOptions]::WriteThrough)
    return [pscustomobject]@{ Stream = $stream; Path = $Path }
}

function Write-Journal {
    <#
      追加一条 JSONL 记录并强制落盘。输入是记录对象；处理过程序列化后写入
      并调用 Flush($true) 穿透文件系统缓存；返回值为空。崩溃归因完全依赖这里。
    #>
    param([Parameter(Mandatory)][hashtable]$Record)

    if ($null -eq $script:Journal) { return }
    $Record['utc'] = Get-UtcStamp
    $line = ($Record | ConvertTo-Json -Compress -Depth 6) + "`n"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($line)
    $script:Journal.Stream.Write($bytes, 0, $bytes.Length)
    $script:Journal.Stream.Flush($true)
}

function Close-Journal {
    if ($null -ne $script:Journal) {
        $script:Journal.Stream.Dispose()
        $script:Journal = $null
    }
}

function Read-JournalRecords {
    <#
      读取一份历史日志。输入是路径；处理过程逐行反序列化并跳过截断的尾行
      （崩溃时最后一行可能只写了一半）；返回值是记录数组。
    #>
    param([Parameter(Mandatory)][string]$Path)

    # 逗号包裹是必要的：PowerShell 会把空数组的返回值退化成 $null。
    if (-not (Test-Path -LiteralPath $Path)) { return , @() }
    $records = @()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $records += ($line | ConvertFrom-Json) } catch { }
    }
    return , $records
}

# ---------------------------------------------------------------------------
# 预检
# ---------------------------------------------------------------------------

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-TestSigningEnabled {
    <#
      判断机器是否处于测试签名模式。输入无；处理过程读取当前启动项的 bcdedit
      输出；返回值是布尔量。未开启时驱动无法加载，本 CI 没有意义。
    #>
    $output = & bcdedit.exe /enum '{current}' 2>&1 | Out-String
    return ($output -match '(?im)^\s*testsigning\s+Yes\s*$')
}

function Assert-CrashDumpConfigured {
    <#
      校验（可选地修正）内核转储配置。输入是是否允许修改；处理过程读取
      CrashControl 注册表项，必要时写入内核转储配置；返回值是配置摘要。
      转储必须可用，否则崩溃后没有可供归因的证据。
    #>
    param([switch]$AllowConfigure)

    $key = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
    $readDword = {
        param($Source, $Name)
        if ($Source.PSObject.Properties.Name -contains $Name) { return [int]$Source.$Name }
        return 0
    }
    $control = Get-ItemProperty -Path $key
    if ($AllowConfigure) {
        Set-ItemProperty -Path $key -Name 'CrashDumpEnabled' -Value 2 -Type DWord
        Set-ItemProperty -Path $key -Name 'AlwaysKeepMemoryDump' -Value 1 -Type DWord
        Set-ItemProperty -Path $key -Name 'AutoReboot' -Value 1 -Type DWord
        $control = Get-ItemProperty -Path $key
    }
    if ((& $readDword $control 'CrashDumpEnabled') -eq 0) {
        throw '机器未开启内核转储（CrashControl\CrashDumpEnabled = 0）。崩溃将无证据可查，请加 -ConfigureCrashDump 或手工开启。'
    }
    return [ordered]@{
        crashDumpEnabled     = (& $readDword $control 'CrashDumpEnabled')
        autoReboot           = (& $readDword $control 'AutoReboot')
        alwaysKeepMemoryDump = (& $readDword $control 'AlwaysKeepMemoryDump')
    }
}

# ---------------------------------------------------------------------------
# 崩溃证据
# ---------------------------------------------------------------------------

function Get-MinidumpInventory {
    <#
      列出当前的内核转储文件。输入无；处理过程枚举 Minidump 目录与 MEMORY.DMP；
      返回值是「路径 -> 最后写入时间」的有序字典，用于与基线比对。
    #>
    $inventory = [ordered]@{}
    $minidumpDir = Join-Path $env:SystemRoot 'Minidump'
    if (Test-Path -LiteralPath $minidumpDir) {
        foreach ($file in Get-ChildItem -LiteralPath $minidumpDir -Filter '*.dmp' -File -ErrorAction SilentlyContinue) {
            $inventory[$file.FullName] = $file.LastWriteTimeUtc.ToString('o')
        }
    }
    $memoryDump = Join-Path $env:SystemRoot 'MEMORY.DMP'
    if (Test-Path -LiteralPath $memoryDump) {
        $inventory[$memoryDump] = (Get-Item -LiteralPath $memoryDump).LastWriteTimeUtc.ToString('o')
    }
    return $inventory
}

function Get-CrashBaseline {
    <#
      采集崩溃判定基线。输入无；处理过程记录当前转储清单、最近一次启动时间和
      System 日志的最新记录号；返回值是基线对象，运行结束后据此求增量。
    #>
    $lastBoot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    $lastRecord = 0
    try {
        $newest = Get-WinEvent -LogName 'System' -MaxEvents 1 -ErrorAction Stop
        $lastRecord = [int64]$newest.RecordId
    } catch { }
    return [pscustomobject]@{
        dumps          = Get-MinidumpInventory
        lastBootUpTime = $lastBoot
        systemRecordId = $lastRecord
        capturedAtUtc  = Get-UtcStamp
    }
}

function Get-CrashEvidence {
    <#
      与基线比对，给出崩溃证据。输入是基线；处理过程比较转储清单、启动时间，
      并查询 BugCheck(1001)/Kernel-Power(41)/EventLog(6008) 三类事件；
      返回值包含 crashed 标志与全部证据明细。
    #>
    param([Parameter(Mandatory)]$Baseline)

    $current = Get-MinidumpInventory
    $newDumps = @()
    foreach ($path in $current.Keys) {
        if (-not $Baseline.dumps.Contains($path) -or $Baseline.dumps[$path] -ne $current[$path]) {
            $newDumps += $path
        }
    }

    $rebooted = $false
    $lastBoot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    if ($lastBoot -ne $Baseline.lastBootUpTime) { $rebooted = $true }

    $events = @()
    $since = [datetime]::Parse($Baseline.capturedAtUtc).ToUniversalTime().AddMinutes(-1)
    foreach ($spec in @(
            @{ Provider = 'Microsoft-Windows-WER-SystemErrorReporting'; Id = 1001 },
            @{ Provider = 'Microsoft-Windows-Kernel-Power'; Id = 41 },
            @{ Provider = 'EventLog'; Id = 6008 })) {
        try {
            $found = Get-WinEvent -FilterHashtable @{
                LogName      = 'System'
                ProviderName = $spec.Provider
                Id           = $spec.Id
                StartTime    = $since
            } -ErrorAction Stop
        } catch {
            continue
        }
        foreach ($entry in $found) {
            $events += [ordered]@{
                provider = $spec.Provider
                id       = $spec.Id
                timeUtc  = $entry.TimeCreated.ToUniversalTime().ToString('o')
                message  = ($entry.Message -replace '\s+', ' ').Trim()
            }
        }
    }

    return [pscustomobject]@{
        crashed        = (($newDumps.Count -gt 0) -or $rebooted -or ($events.Count -gt 0))
        newDumps       = $newDumps
        unexpectedBoot = $rebooted
        events         = $events
        lastBootUpTime = $lastBoot
    }
}

# ---------------------------------------------------------------------------
# 上一轮崩溃阻塞
# ---------------------------------------------------------------------------

function Get-PreviousRunState {
    param([Parameter(Mandatory)][string]$StateRoot)
    $path = Join-Path $StateRoot 'last-run.json'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
}

function Resolve-InFlightCase {
    <#
      从日志推断崩溃时正在执行的用例。输入是日志记录数组；处理过程找出最后一条
      没有配对 case-end 的 case-begin；返回值是该记录或 $null。
    #>
    param([Parameter(Mandatory)][AllowNull()][AllowEmptyCollection()][array]$Records)

    $pending = $null
    if ($null -eq $Records) { return $null }
    foreach ($record in $Records) {
        if ($record.event -eq 'case-begin') { $pending = $record }
        elseif ($record.event -eq 'case-end' -and $null -ne $pending -and $record.case -eq $pending.case) { $pending = $null }
    }
    return $pending
}

function Assert-NoUnresolvedPreviousCrash {
    <#
      阻止在未定位的崩溃之上继续跑新一轮。输入是状态目录与是否已人工确认；
      处理过程检查上一轮是否留下未闭合的日志且伴随崩溃证据；
      返回值为空，命中时抛出并要求 -AcknowledgePreviousCrash。
    #>
    param(
        [Parameter(Mandatory)][string]$StateRoot,
        [switch]$Acknowledged
    )

    $previous = Get-PreviousRunState -StateRoot $StateRoot
    if ($null -eq $previous) { return }
    if ($previous.verdict -ne 'incomplete' -and -not $previous.crash.crashed) { return }

    $journalPath = Join-Path $StateRoot 'journal.jsonl'
    $inFlight = Resolve-InFlightCase -Records (Read-JournalRecords -Path $journalPath)
    $caseText = if ($null -ne $inFlight) { $inFlight.case } else { '<未知>' }

    if ($Acknowledged) {
        Write-Warning "上一轮运行 $($previous.runId) 未正常结束（中断于用例 $caseText），已按 -AcknowledgePreviousCrash 继续。"
        return
    }
    throw ("上一轮运行 $($previous.runId) 未正常结束，中断于用例 $caseText。" +
        "先按 $journalPath 与内核转储定位崩溃，处理完再加 -AcknowledgePreviousCrash 重跑。")
}

# ---------------------------------------------------------------------------
# 驱动服务
# ---------------------------------------------------------------------------

function Install-DriverService {
    <#
      创建并启动被测驱动的内核服务。输入是服务名与 .sys 路径；处理过程先清理
      同名残留，再 sc create/start；返回值为空，失败即抛出。
    #>
    param(
        [Parameter(Mandatory)][string]$ServiceName,
        [Parameter(Mandatory)][string]$DriverPath
    )

    Uninstall-DriverService -ServiceName $ServiceName -Quiet
    Write-Step "创建内核服务 $ServiceName -> $DriverPath"
    $create = & sc.exe create $ServiceName type= kernel start= demand binPath= $DriverPath 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "sc create 失败：$create" }
    $start = & sc.exe start $ServiceName 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Uninstall-DriverService -ServiceName $ServiceName -Quiet
        throw ("sc start 失败：$start" + [Environment]::NewLine +
            '577 通常表示 .sys 未测试签名或证书未进入 LocalMachine\Root 与 TrustedPublisher。')
    }
}

function Uninstall-DriverService {
    param(
        [Parameter(Mandatory)][string]$ServiceName,
        [switch]$Quiet
    )
    if (-not $Quiet) { Write-Step "停止并删除内核服务 $ServiceName" }
    & sc.exe stop $ServiceName 2>&1 | Out-Null
    & sc.exe delete $ServiceName 2>&1 | Out-Null
}

function Test-DriverDeviceAlive {
    <#
      探测驱动控制设备是否仍然可用。输入是 CLI 路径；处理过程发一条最小的只读
      注册表查询；返回值是布尔量，用于在用例之间发现驱动已经失活。
    #>
    param([Parameter(Mandatory)][string]$CliPath)

    $result = Invoke-CliStep -CliPath $CliPath -Arguments @('r0', 'ioctl-registry', '--max-entries', '1') -TimeoutSeconds 30
    return ($result.exitCode -eq 0)
}

# ---------------------------------------------------------------------------
# 用例执行
# ---------------------------------------------------------------------------

function Invoke-CliStep {
    <#
      执行一次 KswordCLI 调用。输入是 CLI 路径、参数数组与超时秒数；处理过程把
      标准输出/错误重定向到临时文件并按超时结束进程；返回值包含退出码、耗时、
      是否超时、是否在超时后仍然无法退出，以及输出尾部。
    #>
    param(
        [Parameter(Mandatory)][string]$CliPath,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][int]$TimeoutSeconds
    )

    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    $timedOut = $false
    $unkillable = $false
    $exitCode = $null
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $process = Start-Process -FilePath $CliPath -ArgumentList $Arguments -PassThru -NoNewWindow `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            # Windows PowerShell 5.1 的 Process.Kill 没有 bool 重载，回退到 taskkill 结束进程树。
            try { $process.Kill() } catch { }
            & taskkill.exe /T /F /PID $process.Id 2>&1 | Out-Null
            # 挂起的 IOCTL 如果没有可用的取消例程，进程会一直卡在内核态无法退出。
            if (-not $process.WaitForExit(30000)) { $unkillable = $true }
        }
        if (-not $unkillable) { $exitCode = $process.ExitCode }
    } finally {
        $watch.Stop()
    }

    $outText = if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue } else { '' }
    $errText = if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue } else { '' }
    Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue

    return [pscustomobject]@{
        exitCode    = $exitCode
        timedOut    = $timedOut
        unkillable  = $unkillable
        elapsedMs   = [int]$watch.ElapsedMilliseconds
        stdout      = if ($null -eq $outText) { '' } else { $outText }
        stderr      = if ($null -eq $errText) { '' } else { $errText }
    }
}

function Test-FaultExitCode {
    <#
      判断退出码是否是用户态异常终止。输入是退出码；处理过程检查是否落在
      NTSTATUS 错误段（0xC0000000 以上）；返回值是布尔量。R3 崩溃同样是缺陷。
    #>
    param($ExitCode)
    if ($null -eq $ExitCode) { return $false }
    # 原生进程的退出码是有符号 Int32，按位重解释成 DWORD 才能和 NTSTATUS 段比较。
    $value = [System.BitConverter]::ToUInt32([System.BitConverter]::GetBytes([int]$ExitCode), 0)
    return ($value -ge [uint32]3221225472)
}

function Expand-PlanArguments {
    <#
      把步骤里的占位符替换成本次运行的实际值。输入是参数数组与变量字典；
      处理过程逐个 token 做整串替换，遇到未定义变量返回 $null；
      返回值是替换后的参数数组或 $null（表示该用例应跳过）。
    #>
    param(
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][hashtable]$Variables
    )

    $expanded = @()
    foreach ($token in $Arguments) {
        $value = $token
        foreach ($match in [regex]::Matches($token, '\{([A-Za-z0-9_.]+)\}')) {
            $name = $match.Groups[1].Value
            if (-not $Variables.ContainsKey($name) -or [string]::IsNullOrEmpty([string]$Variables[$name])) {
                return $null
            }
            $value = $value.Replace($match.Value, [string]$Variables[$name])
        }
        $expanded += $value
    }
    return , $expanded
}

function Assert-TargetGuard {
    <#
      运行时兜底：确认有界写操作确实只打在 harness 自己的对象上。
      输入是 guard 名称、已展开的参数与运行上下文；处理过程按 guard 类型检查
      --pid / --path / --key / --new-name 的取值；返回值为空，越界即抛出。
      这一层是「不许手贱」的第二道防线，静态门禁失效时仍然拦得住。
    #>
    param(
        [Parameter(Mandatory)][string]$Guard,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][hashtable]$Context
    )

    for ($index = 0; $index -lt $Arguments.Count - 1; $index++) {
        $name = $Arguments[$index]
        $value = $Arguments[$index + 1]
        switch ($Guard) {
            'harness-owned-process' {
                if ($name -eq '--pid') {
                    $candidate = 0
                    if (-not [int]::TryParse($value, [ref]$candidate) -or -not $script:SpawnedPids.Contains($candidate)) {
                        throw "targetGuard 违规：$name $value 不是本次运行拉起的进程，拒绝执行。"
                    }
                }
            }
            'harness-owned-path' {
                if ($name -eq '--path') {
                    $normalized = ($value -replace '^\\\?\?\\', '')
                    if (-not $normalized.StartsWith($Context.TempDir, [System.StringComparison]::OrdinalIgnoreCase)) {
                        throw "targetGuard 违规：$name $value 不在本次运行的临时目录内，拒绝执行。"
                    }
                }
            }
            'harness-owned-registry-key' {
                if ($name -eq '--key') {
                    if (-not $value.StartsWith($Context.RegistryTestRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                        throw "targetGuard 违规：$name $value 不在本次运行的测试键内，拒绝执行。"
                    }
                }
                if ($name -eq '--new-name' -and ($value -match '[\\/]')) {
                    throw "targetGuard 违规：--new-name $value 必须是叶名，不能携带路径分隔符。"
                }
            }
            default { throw "未知的 targetGuard：$Guard" }
        }
    }
}

function Invoke-PlanCase {
    <#
      执行一条用例。输入是用例定义、运行上下文与变量字典；处理过程展开参数、
      执行 targetGuard 校验、按顺序跑完全部步骤并始终执行 cleanup；
      返回值是该用例的结果记录。日志的 begin/end 在调用方写入。
    #>
    param(
        [Parameter(Mandatory)]$Case,
        [Parameter(Mandatory)][hashtable]$Context,
        [Parameter(Mandatory)][hashtable]$Variables
    )

    $steps = @()
    $status = 'passed'
    $failureReason = ''

    foreach ($rawStep in $Case.steps) {
        $arguments = Expand-PlanArguments -Arguments ([string[]]$rawStep) -Variables $Variables
        if ($null -eq $arguments) {
            return [ordered]@{
                case = $Case.id; tier = $Case.tier; status = 'skipped'
                reason = '缺少所需的运行时变量'; steps = $steps
            }
        }
        if ($Case.PSObject.Properties.Name -contains 'targetGuard') {
            Assert-TargetGuard -Guard $Case.targetGuard -Arguments $arguments -Context $Context
        }

        $result = Invoke-CliStep -CliPath $Context.CliPath -Arguments $arguments -TimeoutSeconds $Case.timeoutSeconds
        $stepRecord = [ordered]@{
            arguments = ($arguments -join ' ')
            exitCode  = $result.exitCode
            elapsedMs = $result.elapsedMs
            timedOut  = $result.timedOut
            stdoutTail = (($result.stdout -split "`r?`n") | Select-Object -Last 12) -join "`n"
            stderrTail = (($result.stderr -split "`r?`n") | Select-Object -Last 12) -join "`n"
        }
        $steps += $stepRecord

        if ($result.unkillable) {
            $status = 'failed'
            $failureReason = 'pended-request-not-cancelled：请求超时后进程仍无法退出，驱动取消路径存在缺陷。'
            break
        }
        if ($Case.expect -eq 'timeout') {
            if (-not $result.timedOut) {
                $status = 'failed'
                $failureReason = "该用例预期请求会挂起，但进程以 $($result.exitCode) 直接返回。"
                break
            }
            continue
        }
        if ($result.timedOut) {
            $status = 'failed'
            $failureReason = "步骤超时（$($Case.timeoutSeconds)s）：可能是驱动死锁或响应预算失控。"
            break
        }
        if (Test-FaultExitCode -ExitCode $result.exitCode) {
            $status = 'failed'
            $failureReason = "KswordCLI 异常终止，退出码 0x{0:X8}：R3 解析驱动响应时崩溃。" -f ([System.BitConverter]::ToUInt32([System.BitConverter]::GetBytes([int]$result.exitCode), 0))
            break
        }
        if ($Case.expect -eq 'success' -and $result.exitCode -ne 0) {
            $status = 'failed'
            $failureReason = "该用例要求成功，实际退出码 $($result.exitCode)。"
            break
        }
    }

    if ($Case.PSObject.Properties.Name -contains 'cleanup') {
        foreach ($rawStep in $Case.cleanup) {
            $arguments = Expand-PlanArguments -Arguments ([string[]]$rawStep) -Variables $Variables
            if ($null -eq $arguments) { continue }
            try {
                if ($Case.PSObject.Properties.Name -contains 'targetGuard') {
                    Assert-TargetGuard -Guard $Case.targetGuard -Arguments $arguments -Context $Context
                }
                Invoke-CliStep -CliPath $Context.CliPath -Arguments $arguments -TimeoutSeconds 60 | Out-Null
            } catch {
                Write-Warning "用例 $($Case.id) 的清理步骤失败：$($_.Exception.Message)"
            }
        }
    }

    return [ordered]@{
        case = $Case.id; tier = $Case.tier; status = $status
        reason = $failureReason; steps = $steps
    }
}

# ---------------------------------------------------------------------------
# 运行时变量发现
# ---------------------------------------------------------------------------

function Start-SacrificialProcess {
    <#
      拉起一次性目标进程。输入是标签；处理过程启动一个长时间空转的 cmd.exe，
      并把 PID 登记到 SpawnedPids（targetGuard 只认这张表）；返回值是进程对象。
    #>
    param([Parameter(Mandatory)][string]$Label)

    $process = Start-Process -FilePath $env:ComSpec `
        -ArgumentList '/c', 'ping -n 900 127.0.0.1 > nul' `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 400
    $process.Refresh()
    [void]$script:SpawnedPids.Add($process.Id)
    Write-Step "已拉起 $Label 目标进程 PID=$($process.Id)"
    return $process
}

function Stop-SacrificialProcess {
    param([int]$ProcessId)
    if ($ProcessId -le 0) { return }
    & taskkill.exe /T /F /PID $ProcessId 2>&1 | Out-Null
}

function Resolve-RuntimeVariables {
    <#
      构造计划所需的全部运行时变量。输入是运行上下文；处理过程拉起目标进程、
      解析主模块基址与主线程、从 handle enum 输出取一个自有句柄、准备临时文件
      与专属注册表键；返回值是变量字典，缺失项留空并让相关用例记为 skipped。
    #>
    param([Parameter(Mandatory)][hashtable]$Context)

    $variables = @{}
    $variables['self.pid'] = $PID
    $variables['driver.serviceName'] = $Context.ServiceName
    $variables['temp.dirWin32'] = $Context.TempDir
    $variables['system.kernel32Win32'] = (Join-Path $env:SystemRoot 'System32\kernel32.dll')
    $variables['system.kernel32Nt'] = '\??\' + (Join-Path $env:SystemRoot 'System32\kernel32.dll')
    $variables['registry.softwareKeyNt'] = '\REGISTRY\MACHINE\SOFTWARE'
    $variables['registry.currentVersionKeyNt'] = '\REGISTRY\MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $variables['registry.testKeyNt'] = $Context.RegistryTestKey
    $variables['registry.testKeyRenamedLeaf'] = $Context.RegistryTestRenamedLeaf
    $variables['registry.testKeyRenamedNt'] = $Context.RegistryTestRenamedKey

    $probeFile = Join-Path $Context.TempDir 'probe.bin'
    $mutableFile = Join-Path $Context.TempDir 'mutable.bin'
    $deletableFile = Join-Path $Context.TempDir 'deletable.bin'
    $regValueFile = Join-Path $Context.TempDir 'regvalue.bin'
    [System.IO.File]::WriteAllBytes($probeFile, [byte[]](1..64))
    [System.IO.File]::WriteAllBytes($mutableFile, [byte[]](1..64))
    [System.IO.File]::WriteAllBytes($deletableFile, [byte[]](1..64))
    [System.IO.File]::WriteAllBytes($regValueFile, [System.Text.Encoding]::Unicode.GetBytes("KswordARK-CI`0"))
    $variables['temp.probeFileNt'] = '\??\' + $probeFile
    $variables['temp.mutableFileNt'] = '\??\' + $mutableFile
    $variables['temp.deletableFileNt'] = '\??\' + $deletableFile
    $variables['temp.regValueFile'] = $regValueFile

    $target = Start-SacrificialProcess -Label '证据'
    $variables['target.pid'] = $target.Id
    try {
        $variables['target.imageBase'] = '0x' + $target.MainModule.BaseAddress.ToInt64().ToString('X')
    } catch {
        Write-Warning "未能读取目标进程主模块基址：$($_.Exception.Message)"
    }
    try {
        $variables['target.tid'] = ($target.Threads | Select-Object -First 1).Id
    } catch {
        Write-Warning "未能读取目标进程线程列表：$($_.Exception.Message)"
    }

    $handleProbe = Invoke-CliStep -CliPath $Context.CliPath `
        -Arguments @('handle', 'enum', '--pid', "$PID", '--limit', '32') -TimeoutSeconds 120
    $handleMatch = [regex]::Match($handleProbe.stdout, 'handle=0x([0-9a-fA-F]+)')
    if ($handleMatch.Success) {
        $variables['self.handle'] = '0x' + $handleMatch.Groups[1].Value
    } else {
        Write-Warning '未能从 handle enum 输出解析出自有句柄，相关用例将被跳过。'
    }

    $window = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($null -ne $window) {
        $variables['window.hwnd'] = '0x' + $window.MainWindowHandle.ToInt64().ToString('X')
    }

    foreach ($pair in @{
            'profile.v4Blob'        = 'KSWORD_CI_PROFILE_V4_BLOB'
            'callback.rulesBlob'    = 'KSWORD_CI_CALLBACK_RULES_BLOB'
            'redirect.rulesBlob'    = 'KSWORD_CI_REDIRECT_RULES_BLOB'
            'network.rulesBlob'     = 'KSWORD_CI_NETWORK_RULES_BLOB'
            'clipboard.rulesBlob'   = 'KSWORD_CI_CLIPBOARD_POLICY_RULES_BLOB'
            'mutation.targetKind'   = 'KSWORD_CI_MUTATION_TARGET_KIND'
            'mutation.address'      = 'KSWORD_CI_MUTATION_ADDRESS'
            'mutation.beforeHex'    = 'KSWORD_CI_MUTATION_BEFORE_HEX'
            'mutation.afterHex'     = 'KSWORD_CI_MUTATION_AFTER_HEX'
            'mutation.transactionId' = 'KSWORD_CI_MUTATION_TRANSACTION_ID'
        }.GetEnumerator()) {
        $value = [Environment]::GetEnvironmentVariable($pair.Value)
        if (-not [string]::IsNullOrWhiteSpace($value)) { $variables[$pair.Key] = $value }
    }

    return $variables
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

function Invoke-Matrix {
    <#
      执行整轮功能矩阵。输入是解析后的计划与运行上下文；处理过程按计划顺序执行
      用例、周期性探测驱动存活、并把每一步写进崩溃可存活日志；
      返回值是结果数组。
    #>
    param(
        [Parameter(Mandatory)]$Plan,
        [Parameter(Mandatory)][hashtable]$Context,
        [Parameter(Mandatory)][hashtable]$Variables
    )

    $results = @()
    $index = 0
    foreach ($case in $Plan.cases) {
        if ($CaseFilter -and ($case.id -notmatch $CaseFilter)) { continue }
        if ($case.tier -eq 'guarded' -and -not $IncludeGuarded) {
            $results += [ordered]@{ case = $case.id; tier = $case.tier; status = 'skipped'; reason = 'guarded 层默认不执行'; steps = @() }
            continue
        }

        # 生命周期用例作用于一次性目标：目标被终止后换一个新进程，避免相互污染。
        $stepText = ($case.steps | ForEach-Object { $_ -join ' ' }) -join ' '
        if ($stepText -like '*{lifecycle.pid}*') {
            if (-not $Variables.ContainsKey('lifecycle.pid') -or -not $Variables['lifecycle.pid']) {
                $lifecycle = Start-SacrificialProcess -Label '生命周期'
                $Variables['lifecycle.pid'] = $lifecycle.Id
            }
        }

        $index++
        Write-Step "[$index] $($case.id) ($($case.tier))"
        Write-Journal @{ event = 'case-begin'; case = $case.id; tier = $case.tier; ioctls = $case.ioctls }
        if ($case.tier -eq 'guarded') { $script:GuardedExecuted = $true }

        $result = Invoke-PlanCase -Case $case -Context $Context -Variables $Variables
        Write-Journal @{ event = 'case-end'; case = $case.id; status = $result.status; reason = $result.reason }
        $results += $result

        if ($result.status -eq 'failed') {
            Write-Warning "用例 $($case.id) 失败：$($result.reason)"
        }
        # 目标进程被终止后，后续生命周期用例需要新的目标。
        if ($case.id -eq 'process.terminate') { $Variables['lifecycle.pid'] = $null }

        if (($index % 10) -eq 0) {
            if (-not (Test-DriverDeviceAlive -CliPath $Context.CliPath)) {
                Write-Journal @{ event = 'device-lost'; afterCase = $case.id }
                throw "驱动控制设备在用例 $($case.id) 之后不再响应，终止本轮。"
            }
        }
    }
    return $results
}

function Resolve-Attribution {
    <#
      给崩溃定性。输入是崩溃证据、执行日志与上一轮状态；处理过程按「崩溃时在跑
      哪个用例」「本轮/上轮是否开过 guarded」逐级判断；返回值是归因字符串。
      driver-defect 表示计划内的安全操作把机器打崩了，属于真实驱动缺陷。
    #>
    param(
        [Parameter(Mandatory)]$Crash,
        [Parameter(Mandatory)][AllowNull()][AllowEmptyCollection()][array]$Records,
        $PreviousState
    )

    if (-not $Crash.crashed) { return 'none' }
    $inFlight = Resolve-InFlightCase -Records $Records
    if ($null -ne $inFlight -and $inFlight.tier -eq 'guarded') { return 'harness-attributable' }
    if ($script:GuardedExecuted) { return 'possibly-self-inflicted-this-run' }
    if ($null -ne $PreviousState -and $PreviousState.PSObject.Properties.Name -contains 'guardedExecuted' `
            -and $PreviousState.guardedExecuted `
            -and $PreviousState.crash.lastBootUpTime -eq $Crash.lastBootUpTime) {
        # PatchGuard 的检查是延迟触发的：同一次启动里跑过 guarded，就不能把崩溃算到本轮头上。
        return 'possibly-self-inflicted-previous-run'
    }
    return 'driver-defect'
}

function Invoke-SelfTest {
    <#
      自检 harness 的安全与归因逻辑。输入是计划路径；处理过程用合成输入验证
      targetGuard 会拦下越界目标、占位符展开会在变量缺失时跳过、崩溃归因会区分
      guarded 与 probe，并复核计划里 probe 层没有夹带危险 IOCTL；
      返回值是退出码。该模式不加载驱动，也不要求管理员或测试签名。
    #>
    param([Parameter(Mandatory)][string]$PlanPath)

    $failures = @()
    function Assert-True {
        param([bool]$Condition, [string]$Message)
        if (-not $Condition) { $script:SelfTestFailures += $Message }
    }
    $script:SelfTestFailures = @()

    $context = @{
        CliPath = 'unused'; ServiceName = 'KswordARK'
        TempDir = 'C:\Temp\KswordArkDriverCI-selftest'
        RegistryTestRoot = '\REGISTRY\MACHINE\SOFTWARE\KswordARKDriverCI'
        RegistryTestKey = '\REGISTRY\MACHINE\SOFTWARE\KswordARKDriverCI\run-selftest'
        RegistryTestRenamedLeaf = 'run-selftest-renamed'
        RegistryTestRenamedKey = '\REGISTRY\MACHINE\SOFTWARE\KswordARKDriverCI\run-selftest-renamed'
    }

    # 1. 进程 guard 只认本次运行拉起的 PID。
    [void]$script:SpawnedPids.Add(424242)
    $blocked = $false
    try { Assert-TargetGuard -Guard 'harness-owned-process' -Arguments @('process', 'terminate', '--pid', '4') -Context $context }
    catch { $blocked = $true }
    Assert-True $blocked 'targetGuard 未能拦下指向系统进程（PID 4）的终止请求。'

    $allowed = $true
    try { Assert-TargetGuard -Guard 'harness-owned-process' -Arguments @('process', 'terminate', '--pid', '424242') -Context $context }
    catch { $allowed = $false }
    Assert-True $allowed 'targetGuard 错误地拦下了本次运行自己拉起的进程。'
    [void]$script:SpawnedPids.Remove(424242)

    # 2. 路径 guard 只认本次运行的临时目录，NT 前缀也要能识别。
    $blocked = $false
    try { Assert-TargetGuard -Guard 'harness-owned-path' -Arguments @('file', 'delete-path', '--path', '\??\C:\Windows\System32\ntoskrnl.exe') -Context $context }
    catch { $blocked = $true }
    Assert-True $blocked 'targetGuard 未能拦下指向系统目录的删除请求。'

    $allowed = $true
    try { Assert-TargetGuard -Guard 'harness-owned-path' -Arguments @('file', 'delete-path', '--path', ('\??\' + $context.TempDir + '\deletable.bin')) -Context $context }
    catch { $allowed = $false }
    Assert-True $allowed 'targetGuard 错误地拦下了本次运行的临时文件。'

    # 3. 注册表 guard 只认专属测试键。
    $blocked = $false
    try { Assert-TargetGuard -Guard 'harness-owned-registry-key' -Arguments @('registry', 'delete-key', '--key', '\REGISTRY\MACHINE\SYSTEM\CurrentControlSet') -Context $context }
    catch { $blocked = $true }
    Assert-True $blocked 'targetGuard 未能拦下指向 CurrentControlSet 的删除请求。'

    # 4. 占位符缺失时用例应跳过而不是发出半成品命令。
    $expanded = Expand-PlanArguments -Arguments @('process', 'detail', '--pid', '{target.pid}') -Variables @{}
    Assert-True ($null -eq $expanded) '变量缺失时占位符展开没有返回 $null。'
    $expanded = Expand-PlanArguments -Arguments @('process', 'detail', '--pid', '{target.pid}') -Variables @{ 'target.pid' = 1234 }
    Assert-True (($expanded -join ' ') -eq 'process detail --pid 1234') '占位符展开结果不正确。'

    # 5. 异常退出码要能识别成 R3 崩溃。
    Assert-True (Test-FaultExitCode -ExitCode -1073741819) '未能把 0xC0000005 判定为异常终止。'
    Assert-True (-not (Test-FaultExitCode -ExitCode 3)) '把普通非零退出码误判成了异常终止。'

    # 6. 崩溃归因：guarded 用例中崩溃属于 harness 自伤，probe 用例中崩溃才是驱动缺陷。
    $crash = [pscustomobject]@{ crashed = $true; lastBootUpTime = 'b1' }
    $guardedRecords = @(
        [pscustomobject]@{ event = 'case-begin'; case = 'guarded.x'; tier = 'guarded' })
    $script:GuardedExecuted = $true
    Assert-True ((Resolve-Attribution -Crash $crash -Records $guardedRecords -PreviousState $null) -eq 'harness-attributable') `
        'guarded 用例执行期间的崩溃未被归因为 harness-attributable。'
    $script:GuardedExecuted = $false
    $probeRecords = @(
        [pscustomobject]@{ event = 'case-begin'; case = 'probe.x'; tier = 'probe' })
    Assert-True ((Resolve-Attribution -Crash $crash -Records $probeRecords -PreviousState $null) -eq 'driver-defect') `
        'probe 用例执行期间的崩溃未被归因为 driver-defect。'
    $previous = [pscustomobject]@{ guardedExecuted = $true; crash = [pscustomobject]@{ lastBootUpTime = 'b1' } }
    Assert-True ((Resolve-Attribution -Crash $crash -Records $probeRecords -PreviousState $previous) -eq 'possibly-self-inflicted-previous-run') `
        '同一次启动内上轮跑过 guarded，本轮崩溃未被降级归因（PatchGuard 延迟触发窗口）。'
    Assert-True ((Resolve-Attribution -Crash ([pscustomobject]@{ crashed = $false; lastBootUpTime = 'b1' }) -Records $probeRecords -PreviousState $null) -eq 'none') `
        '未崩溃时归因不应为 none 之外的值。'

    # 7. 崩溃时的在途用例要能从半截日志里还原。
    $records = @(
        [pscustomobject]@{ event = 'case-begin'; case = 'a'; tier = 'probe' },
        [pscustomobject]@{ event = 'case-end'; case = 'a'; status = 'passed' },
        [pscustomobject]@{ event = 'case-begin'; case = 'b'; tier = 'probe' })
    $inFlight = Resolve-InFlightCase -Records $records
    Assert-True ($null -ne $inFlight -and $inFlight.case -eq 'b') '未能从日志还原崩溃时的在途用例。'

    # 8. 上一轮崩溃未定位时必须阻塞新一轮，确认后才放行。
    $selfTestState = Join-Path ([System.IO.Path]::GetTempPath()) ("KswordArkDriverCI-selftest-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $selfTestState -Force | Out-Null
    try {
        '{"event":"case-begin","case":"memory.read-phys","tier":"probe"}' |
            Set-Content -LiteralPath (Join-Path $selfTestState 'journal.jsonl') -Encoding UTF8
        '{"runId":"selftest","verdict":"incomplete","guardedExecuted":false,"crash":{"crashed":false,"lastBootUpTime":"b1"}}' |
            Set-Content -LiteralPath (Join-Path $selfTestState 'last-run.json') -Encoding UTF8

        $blocked = $false
        try { Assert-NoUnresolvedPreviousCrash -StateRoot $selfTestState } catch { $blocked = $true }
        Assert-True $blocked '上一轮留下未闭合日志时没有阻塞新一轮运行。'

        $released = $true
        try { Assert-NoUnresolvedPreviousCrash -StateRoot $selfTestState -Acknowledged } catch { $released = $false }
        Assert-True $released '-AcknowledgePreviousCrash 未能解除阻塞。'

        '{"runId":"selftest","verdict":"passed","guardedExecuted":false,"crash":{"crashed":false,"lastBootUpTime":"b1"}}' |
            Set-Content -LiteralPath (Join-Path $selfTestState 'last-run.json') -Encoding UTF8
        $released = $true
        try { Assert-NoUnresolvedPreviousCrash -StateRoot $selfTestState } catch { $released = $false }
        Assert-True $released '上一轮正常结束时不应阻塞新一轮运行。'
    } finally {
        Remove-Item -LiteralPath $selfTestState -Recurse -Force -ErrorAction SilentlyContinue
    }

    # 9. 计划自身复核：probe 层不得夹带命中危险模式的 IOCTL。
    $plan = Get-Content -LiteralPath $PlanPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $patterns = @($plan.policy.mustExcludePatterns)
    foreach ($case in $plan.cases) {
        foreach ($ioctl in $case.ioctls) {
            foreach ($pattern in $patterns) {
                if ($ioctl -match $pattern) {
                    $script:SelfTestFailures += "用例 $($case.id) 引用了命中危险模式的 $ioctl。"
                }
            }
        }
    }

    $failures = $script:SelfTestFailures
    foreach ($message in $failures) { Write-Host "  SELFTEST FAIL $message" }
    if ($failures.Count -gt 0) {
        Write-Host "harness 自检失败：$($failures.Count) 项。"
        return $script:ExitCaseFailure
    }
    Write-Host 'harness 自检通过：targetGuard、占位符、退出码判定、崩溃归因与计划复核均符合预期。'
    return $script:ExitPass
}

function Main {
    if (-not $RepositoryRoot) {
        $RepositoryRoot = (Resolve-Path (Join-Path $script:ScriptDirectory '..\..')).Path
    }
    if (-not $StateRoot) {
        $StateRoot = Join-Path $env:ProgramData 'KswordARK\driver-functional-ci'
    }
    if (-not $PlanPath) {
        $PlanPath = Join-Path $RepositoryRoot 'tools\driver_functional_ci\driver_test_plan.json'
    }
    if (-not $BinaryDir) {
        $BinaryDir = Join-Path $RepositoryRoot 'Ksword5.1\x64\Release'
    }
    if (-not $ArtifactRoot) {
        $ArtifactRoot = Join-Path $RepositoryRoot 'artifacts\driver-functional-ci'
    }
    if (-not (Test-Path -LiteralPath $ArtifactRoot)) {
        New-Item -ItemType Directory -Path $ArtifactRoot -Force | Out-Null
    }

    # 自检不加载驱动，也不需要管理员或测试签名，因此在所有环境校验之前分流。
    if ($Mode -eq 'SelfTest') {
        return (Invoke-SelfTest -PlanPath $PlanPath)
    }
    if (-not (Test-Path -LiteralPath $StateRoot)) {
        New-Item -ItemType Directory -Path $StateRoot -Force | Out-Null
    }

    $runId = [Guid]::NewGuid().ToString('N').Substring(0, 12)
    $previousState = Get-PreviousRunState -StateRoot $StateRoot
    $journalPath = Join-Path $StateRoot 'journal.jsonl'
    $verdictPath = Join-Path $ArtifactRoot 'verdict.json'

    if ($Mode -eq 'Verify') {
        $records = Read-JournalRecords -Path $journalPath
        $inFlight = Resolve-InFlightCase -Records $records
        $verdict = [ordered]@{
            runId = $runId; mode = 'Verify'
            previousRunId = if ($null -ne $previousState) { $previousState.runId } else { $null }
            inFlightCase = if ($null -ne $inFlight) { $inFlight.case } else { $null }
            previousVerdict = if ($null -ne $previousState) { $previousState.verdict } else { $null }
        }
        $verdict | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $verdictPath -Encoding UTF8
        Write-Step "Verify 模式完成，结果见 $verdictPath"
        if ($null -ne $previousState -and $previousState.verdict -eq 'incomplete') { return $script:ExitCrash }
        return $script:ExitPass
    }

    if (-not (Test-Administrator)) {
        throw '必须以管理员身份运行：加载内核驱动与读取崩溃证据都需要管理员权限。'
    }
    if (-not (Test-TestSigningEnabled)) {
        throw '当前机器未开启测试签名模式（bcdedit /set testsigning on 并重启），无法加载测试签名驱动。'
    }
    if ($IncludeGuarded -and -not $AcknowledgeGuardedRisk) {
        throw '-IncludeGuarded 必须与 -AcknowledgeGuardedRisk 一起使用：guarded 层会让崩溃归因降级为 harness-attributable。'
    }
    Assert-NoUnresolvedPreviousCrash -StateRoot $StateRoot -Acknowledged:$AcknowledgePreviousCrash

    $cliPath = Join-Path $BinaryDir 'KswordCLI.exe'
    $driverPath = Join-Path $BinaryDir 'KswordARK.sys'
    foreach ($required in @($PlanPath, $cliPath, $driverPath)) {
        if (-not (Test-Path -LiteralPath $required)) { throw "缺少必需文件：$required" }
    }
    $dumpConfig = Assert-CrashDumpConfigured -AllowConfigure:$ConfigureCrashDump

    $plan = Get-Content -LiteralPath $PlanPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($plan.schemaVersion -ne 1) {
        throw "不认识的计划 schemaVersion=$($plan.schemaVersion)，请同步更新本 runner。"
    }
    $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) "KswordArkDriverCI-$runId"
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

    $context = @{
        CliPath                 = $cliPath
        ServiceName             = $ServiceName
        TempDir                 = $tempDir
        RegistryTestRoot        = '\REGISTRY\MACHINE\SOFTWARE\KswordARKDriverCI'
        RegistryTestKey         = "\REGISTRY\MACHINE\SOFTWARE\KswordARKDriverCI\run-$runId"
        RegistryTestRenamedLeaf = "run-$runId-renamed"
        RegistryTestRenamedKey  = "\REGISTRY\MACHINE\SOFTWARE\KswordARKDriverCI\run-$runId-renamed"
    }

    $baseline = Get-CrashBaseline
    $script:Journal = Open-Journal -Path $journalPath
    Write-Journal @{
        event = 'run-begin'; runId = $runId; plan = $PlanPath
        includeGuarded = [bool]$IncludeGuarded; lastBootUpTime = $baseline.lastBootUpTime
        driver = $driverPath; cli = $cliPath
    }

    $state = [ordered]@{
        runId = $runId; startedUtc = Get-UtcStamp; verdict = 'incomplete'
        guardedExecuted = $false; crash = @{ crashed = $false; lastBootUpTime = $baseline.lastBootUpTime }
    }
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $StateRoot 'last-run.json') -Encoding UTF8

    $results = @()
    $runError = $null
    try {
        if (-not $SkipDriverLoad) {
            Install-DriverService -ServiceName $ServiceName -DriverPath $driverPath
        }
        if (-not (Test-DriverDeviceAlive -CliPath $cliPath)) {
            throw '驱动已加载但控制设备不可用，KswordCLI 无法打开 \\.\KswordARKLog。'
        }
        $variables = Resolve-RuntimeVariables -Context $context
        $results = Invoke-Matrix -Plan $plan -Context $context -Variables $variables
    } catch {
        $runError = $_.Exception.Message
        Write-Warning "本轮执行中断：$runError"
    } finally {
        foreach ($spawned in @($script:SpawnedPids)) { Stop-SacrificialProcess -ProcessId $spawned }
        if (-not $SkipDriverLoad) { Uninstall-DriverService -ServiceName $ServiceName }
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath 'HKLM:\SOFTWARE\KswordARKDriverCI' -Recurse -Force -ErrorAction SilentlyContinue
    }

    $crash = Get-CrashEvidence -Baseline $baseline
    Write-Journal @{ event = 'run-end'; runId = $runId; crashed = $crash.crashed }
    Close-Journal

    $records = Read-JournalRecords -Path $journalPath
    $attribution = Resolve-Attribution -Crash $crash -Records $records -PreviousState $previousState

    $failed = @($results | Where-Object { $_.status -eq 'failed' })
    $skipped = @($results | Where-Object { $_.status -eq 'skipped' })
    $skippedProbe = @($skipped | Where-Object { $_.tier -eq 'probe' })
    $passed = @($results | Where-Object { $_.status -eq 'passed' })

    $exitCode = $script:ExitPass
    $verdictName = 'passed'
    if ($crash.crashed) {
        $exitCode = $script:ExitCrash
        $verdictName = "crashed:$attribution"
    } elseif ($null -ne $runError) {
        $exitCode = $script:ExitCaseFailure
        $verdictName = 'aborted'
    } elseif ($failed.Count -gt 0) {
        $exitCode = $script:ExitCaseFailure
        $verdictName = 'failed'
    } elseif ($FailOnSkippedProbe -and $skippedProbe.Count -gt 0) {
        $exitCode = $script:ExitCaseFailure
        $verdictName = 'failed:skipped-probe'
    }

    $verdict = [ordered]@{
        runId               = $runId
        verdict             = $verdictName
        attribution         = $attribution
        attributionWeakened = [bool]$script:GuardedExecuted
        crash               = $crash
        dumpConfiguration   = $dumpConfig
        abortReason         = $runError
        counts              = [ordered]@{
            total = $results.Count; passed = $passed.Count; failed = $failed.Count
            skipped = $skipped.Count; skippedProbe = $skippedProbe.Count
        }
        results             = $results
        journal             = $journalPath
    }
    $verdict | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $verdictPath -Encoding UTF8

    $state['verdict'] = $verdictName
    $state['guardedExecuted'] = [bool]$script:GuardedExecuted
    $state['crash'] = @{ crashed = $crash.crashed; lastBootUpTime = $crash.lastBootUpTime }
    $state['finishedUtc'] = Get-UtcStamp
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $StateRoot 'last-run.json') -Encoding UTF8

    Write-Host ''
    Write-Host "结果：$verdictName（通过 $($passed.Count) / 失败 $($failed.Count) / 跳过 $($skipped.Count)）"
    if ($crash.crashed) {
        $inFlight = Resolve-InFlightCase -Records $records
        Write-Host "系统崩溃证据：转储 $($crash.newDumps.Count) 个，事件 $($crash.events.Count) 条，意外重启=$($crash.unexpectedBoot)"
        Write-Host "崩溃时正在执行：$(if ($null -ne $inFlight) { $inFlight.case } else { '<无用例在执行>' })"
        Write-Host "归因：$attribution"
    }
    foreach ($item in $failed) { Write-Host "  FAIL $($item.case): $($item.reason)" }
    Write-Host "判定文件：$verdictPath"
    return $exitCode
}

try {
    $mainResult = Main
    # Main 只应返回退出码；万一有函数向管道泄漏了对象，取最后一个值兜底。
    if ($mainResult -is [array]) { $mainResult = $mainResult[-1] }
    exit ([int]$mainResult)
} catch {
    Close-Journal
    # 这里不能用 Write-Error：ErrorActionPreference=Stop 会让它自身抛出，
    # 脚本会以 1 结束而不是预检专用的退出码。
    $Host.UI.WriteErrorLine("[driver-matrix] $($_.Exception.Message)")
    exit $script:ExitPreflight
}
