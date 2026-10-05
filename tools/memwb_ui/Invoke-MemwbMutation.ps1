# Invoke-MemwbMutation.ps1 —— 内存工作台离屏夹具的"副本变异"重放工具（需要 PowerShell 7+，即 pwsh）。
#
# 为什么需要它：夹具"全绿"不等于夹具"有牙齿"。独立审核的做法是：在源码副本上做一个最小的行为改动
# （条件取反、边界、早返回删除、字段错配……），看夹具会不会失败；不失败=该判断没有测试盯着。
# 本工具把"建副本→改一处→完整构建夹具→判定"做成一条命令，供实现者自检与独立审核者复核。
#
# 用法（在任意目录）：
#   pwsh -NoProfile -File tools\memwb_ui\Invoke-MemwbMutation.ps1 -Pkg wpE -Id M1 `
#        -File "Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookModel.cpp" -From "旧文本" -To "新文本"
#   pwsh -NoProfile -File tools\memwb_ui\Invoke-MemwbMutation.ps1 -Pkg wpE -Id C0 -Control   # 对照组，变异前必须先过
#
# 规则与判据：
#   - 只改副本（.codex-tmp\memwb-mut\<Id>\，已被 gitignore），绝不改仓库文件；.deps 与 Resource 用目录联接只读使用。
#   - From 必须在目标文件里恰好出现 1 次（序号比较，保持原文件换行），否则 NOT_APPLIED。
#   - 结果一行：RESULT=<CAUGHT|SURVIVED|COMPILE_ERROR|CRASH_OR_NOSUMMARY|NOT_APPLIED|CONTROL_OK|CONTROL_FAIL|TIMEOUT>。
#     CAUGHT/TIMEOUT/CRASH_OR_NOSUMMARY 都算"已暴露"；SURVIVED 必须判断是夹具缺口还是等价变异。
#   - 通过判据只认各包最终汇总行；判定失败行用区分大小写的 FAIL，避免把子套件的 "failures=0" 当成失败。
#   - 退出码在 Start-Process 后可能读成空，所以先缓存句柄，读不到时按日志判定。
#   - /W4 /WX 下常量条件（if (true)）与未使用变量会编不过，变异请写成非常量形式（x || y）。
#   - 超时只杀自己启动的进程树；跑完立即删副本（-Keep 保留）。变异请逐个串行跑（构建产物较大）。
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^wp[A-Z][0-9A-Za-z]*$')][string]$Pkg,
    [Parameter(Mandatory = $true)][string]$Id,
    [string]$File = '',
    [string]$From = '',
    [string]$To = '',
    [switch]$Control,
    [switch]$Keep,
    [int]$TimeoutSec = 300
)
$ErrorActionPreference = 'Continue'
# 仓库根：本脚本位于 <repo>\tools\memwb_ui\，向上两级。
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$root = Join-Path $repo ".codex-tmp\memwb-mut\$Id"

function Remove-Tree([string]$path) {
    # 先单独摘掉联接（非递归删除只删链接本身，不会进入 Qt/图标目录），再递归删其余。
    foreach ($rel in '.deps', 'Ksword5.1\Ksword5.1\Resource') {
        $junction = Join-Path $path $rel
        if (Test-Path -LiteralPath $junction) { try { [IO.Directory]::Delete($junction) } catch { } }
    }
    if (Test-Path -LiteralPath $path) { try { [IO.Directory]::Delete($path, $true) } catch { Write-Output "WARN: 清理失败 $path : $_" } }
}

Remove-Tree $root
New-Item -ItemType Directory -Force -Path $root | Out-Null

# 复制最小源码子树（robocopy 退出码 <8 都算成功）。
$copies = @(
    @('Ksword5.1\Ksword5.1\UI', '/E'),
    @('Ksword5.1\Ksword5.1\Internationalization', '/E'),
    @('shared', '/E'),
    @('tools\memwb_ui', '/E')
)
foreach ($c in $copies) {
    robocopy (Join-Path $repo $c[0]) (Join-Path $root $c[0]) $c[1] /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
}
robocopy (Join-Path $repo 'Ksword5.1\Ksword5.1') (Join-Path $root 'Ksword5.1\Ksword5.1') '*.h' '*.hpp' /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
# 装配层夹具借用 Light 测试里的假端口等支持头（Qt-free，只读）。
robocopy (Join-Path $repo 'KswordARKLightTests') (Join-Path $root 'KswordARKLightTests') '*.h' /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
# 主程序 MemoryDock 下的真实端口与服务（头文件；以及 Workbench* 开头的实现，供链接它们的夹具使用）。
robocopy (Join-Path $repo 'Ksword5.1\Ksword5.1\MemoryDock') (Join-Path $root 'Ksword5.1\Ksword5.1\MemoryDock') '*.h' 'Workbench*.cpp' 'MemoryDock.Workbench*.cpp' /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
# 项目工具库头文件 + 日志实现（Win32 + std only）：MemoryDock 侧生产审计接收器的夹具要链接真实日志仓库。
robocopy (Join-Path $repo 'Ksword5.1\Ksword5.1') (Join-Path $root 'Ksword5.1\Ksword5.1') '*.h' '*.hpp' /E /XD x64 Resource languages .vs Generated /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
robocopy (Join-Path $repo 'Ksword5.1\Ksword5.1\ksword\log') (Join-Path $root 'Ksword5.1\Ksword5.1\ksword\log') '*.cpp' /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
if ($Pkg -eq 'wpH' -or $Pkg -eq 'wpJ6') {
    # 这两个夹具链接 CodeEditorWidget 依赖链，需要 Zydis.c。
    robocopy (Join-Path $repo 'third_party\zydis') (Join-Path $root 'third_party\zydis') /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
}
New-Item -ItemType Junction -Path (Join-Path $root '.deps') -Target (Join-Path $repo '.deps') | Out-Null
# 图标资源（qrc 引用的 SVG）只读使用，用联接代替复制数千个小文件。
New-Item -ItemType Junction -Path (Join-Path $root 'Ksword5.1\Ksword5.1\Resource') -Target (Join-Path $repo 'Ksword5.1\Ksword5.1\Resource') | Out-Null

# 应用变异（保持原文件的换行与编码，按 UTF-8 无 BOM 读写）。
if (-not $Control) {
    $target = Join-Path $root $File
    if (-not (Test-Path -LiteralPath $target)) { Write-Output "RESULT=NOT_APPLIED id=$Id reason=文件不存在: $File"; Remove-Tree $root; exit 3 }
    $enc = New-Object System.Text.UTF8Encoding($false)
    $text = [IO.File]::ReadAllText($target, $enc)
    $count = 0; $pos = 0
    while (($pos = $text.IndexOf($From, $pos, [StringComparison]::Ordinal)) -ge 0) { $count++; $pos += $From.Length }
    if ($count -ne 1) { Write-Output "RESULT=NOT_APPLIED id=$Id reason=From 在文件中出现 $count 次(要求恰好1次)"; Remove-Tree $root; exit 3 }
    $idx = $text.IndexOf($From, [StringComparison]::Ordinal)
    $mutated = $text.Substring(0, $idx) + $To + $text.Substring($idx + $From.Length)
    [IO.File]::WriteAllText($target, $mutated, $enc)
}

# 构建并运行：MEMWB_OUT 只放进本次 cmd 命令行；超时只杀自己启动的进程树。
$out = Join-Path $root 'out'
$log = Join-Path $root 'run.log'
$cmdLine = "cd /d `"$root`" && set `"MEMWB_OUT=$out`"&& call tools\memwb_ui\$Pkg\build-$Pkg-tests.cmd"
$proc = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', $cmdLine -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError (Join-Path $root 'run.err')
# 必须先取一次 Handle 缓存进程句柄，否则进程退出后 ExitCode 可能读到空值（PowerShell 已知坑）。
$null = $proc.Handle
if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    & taskkill /T /F /PID $proc.Id | Out-Null
    Write-Output "RESULT=TIMEOUT id=$Id (运行超过 $TimeoutSec 秒，按死循环类缺陷处理=已被暴露)"
    if (-not $Keep) { Remove-Tree $root }
    exit 2
}
$proc.WaitForExit()   # 无参重载：等重定向流读完
$exit = $proc.ExitCode
if ($null -eq $exit -or "$exit" -eq '') { $exit = -1 }   # 读不到退出码时退化为按日志判定
$logText = ''
Start-Sleep -Milliseconds 500   # 等重定向句柄释放
foreach ($f in @($log, (Join-Path $root 'run.err'))) {
    if (-not (Test-Path -LiteralPath $f)) { continue }
    # 以共享读方式打开，避免日志句柄尚未释放时读失败。
    $fs = [IO.File]::Open($f, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
    $logText += $sr.ReadToEnd() + "`n"
    $sr.Dispose()
}

$compileErr = $logText -match 'error C\d+|fatal error|error LNK\d+|error RC\d+'
# 失败数两种格式：「N failures」（E/F/I 风格）与「failures=N」（H 风格）；
# 前者要排除「checks=45 failures=0」里 "45 failures" 的误读（后面紧跟 = 的不算）。
$anyFail = $false
foreach ($m in [regex]::Matches($logText, '(\d+)\s+failures?\b(?!=)')) { if ([int]$m.Groups[1].Value -gt 0) { $anyFail = $true } }
foreach ($m in [regex]::Matches($logText, 'failures=(\d+)')) { if ([int]$m.Groups[1].Value -gt 0) { $anyFail = $true } }
# 通过判据只认各包"最终汇总行"（中间某个子套件的 0 failures 不算；进程中途崩溃时不会有最终行）。
$finalPass = @{
    wpE = 'wpE_tests: \d+ checks, 0 failures'
    wpF = 'wpF_tests: \d+ checks, 0 failures'
    wpG = 'wpG_tests: all \d+ checks passed'
    wpH = '\[wpH\] total checks=\d+ failures=0'
    wpI = 'memwb_ui_tests: \d+ checks, 0 failures'
}
# 装配层夹具（wpJ*）与之后的包统一用「<pkg>_tests: N checks, 0 failures」作最终汇总行。
$finalPattern = if ($finalPass.ContainsKey($Pkg)) { $finalPass[$Pkg] } else { "$Pkg" + '_tests: \d+ checks, 0 failures' }
$passSummary = $logText -match $finalPattern
# 失败行用区分大小写的 FAIL（夹具打印 "FAIL:" / "[FAIL]"），避免命中 "failures=0" 里的小写 fail。
$failLine = $logText -cmatch 'FAIL'

if ($compileErr) {
    $result = 'COMPILE_ERROR'
}
elseif ($passSummary -and -not $anyFail -and ($exit -eq 0 -or $exit -eq -1)) {
    # 日志里有"0 failures"最终汇总且没有任何失败行：以日志为准（退出码只作旁证，读不到时为 -1）。
    $result = if ($Control) { 'CONTROL_OK' } else { 'SURVIVED' }
}
elseif ($anyFail -or $failLine) {
    $result = if ($Control) { 'CONTROL_FAIL' } else { 'CAUGHT' }
}
else {
    $result = if ($Control) { 'CONTROL_FAIL' } else { 'CRASH_OR_NOSUMMARY' }
}
$fails = ($logText -split "`n" | Where-Object { $_ -cmatch 'FAIL' } | Select-Object -First 3) -join ' | '
Write-Output "RESULT=$result id=$Id exit=$exit pkg=$Pkg"
# 附带输出该包最终汇总行（例如 "wpJ6_tests: 526 checks, 0 failures"），对照组时能直接看到断言总数。
$summaryLine = $logText -split "`n" | Where-Object { $_ -match $finalPattern } | Select-Object -Last 1
if ($summaryLine) { Write-Output ("  summary: " + $summaryLine.Trim()) }
if ($result -in 'CAUGHT', 'CRASH_OR_NOSUMMARY', 'CONTROL_FAIL', 'COMPILE_ERROR') {
    Write-Output ("  首批失败行: " + $fails)
    if ($result -in 'CRASH_OR_NOSUMMARY', 'CONTROL_FAIL', 'COMPILE_ERROR') {
        Write-Output ("  日志末尾: " + (($logText -split "`n" | Select-Object -Last 6) -join ' | '))
    }
}
if (-not $Keep) { Remove-Tree $root }
