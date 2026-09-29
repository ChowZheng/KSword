<#
.SYNOPSIS
    核验 KswordARKLightTests 的输出里，清单上列的每个套件都真的跑过了。

.DESCRIPTION
    只看退出码抓不到「整个套件没被链接进来」这一类故障：没进 .vcxproj 的套件
    一条断言都不会跑，而测试进程照样退出 0、照样打印「全部通过」。
    HvmEptSwitchTests 就这样静默缺席过一整轮 —— 344 条断言写完了，一次没跑。

    所以判据不是退出码，而是三条：
      * 清单里的套件没出现在输出里            -> FAIL
      * 出现了但断言数低于 minAssertions      -> FAIL（有人把测试删了）
      * 出现了清单外的新套件                  -> 只警告（新增套件是好事，
                                                 只是要有人来更新清单）

.PARAMETER TestExePath
    KswordARKLightTests.exe 的路径。给了就由本脚本执行它。

.PARAMETER OutputPath
    已经跑过一次、输出存下来的文本文件。与 -TestExePath 二选一。

.PARAMETER ManifestPath
    套件清单，默认 scripts/ksword-expected-suites.json。

.OUTPUTS
    退出码 0 = 全部核对通过；1 = 有套件缺席或断言数缩水。
#>
[CmdletBinding()]
param(
    [string] $TestExePath,
    [string] $OutputPath,
    [string] $ManifestPath = (Join-Path $PSScriptRoot 'ksword-expected-suites.json')
)

$ErrorActionPreference = 'Stop'

# 两个输入源二选一：要么本脚本跑一次，要么读别人跑好的输出。
if (-not $TestExePath -and -not $OutputPath) {
    throw '必须给 -TestExePath 或 -OutputPath 其中之一。'
}
if (-not (Test-Path -LiteralPath $ManifestPath)) {
    throw "套件清单不存在：$ManifestPath（无法核验套件是否齐全）"
}

# 取得测试输出。自己跑时退出码单独留着，它与清单核验是两条独立判据。
$runExit = 0
if ($TestExePath) {
    if (-not (Test-Path -LiteralPath $TestExePath)) {
        throw "测试程序不存在：$TestExePath"
    }
    $lines = & $TestExePath 2>&1 | ForEach-Object { [string] $_ }
    $runExit = $LASTEXITCODE
    $lines | ForEach-Object { Write-Host $_ }
}
else {
    if (-not (Test-Path -LiteralPath $OutputPath)) {
        throw "测试输出不存在：$OutputPath"
    }
    $lines = Get-Content -LiteralPath $OutputPath
}

# 解析每个套件那一行，形如 "  HVM watch: 157/157 checks passed"。
$suites = @{}
foreach ($line in $lines) {
    $match = [regex]::Match(
        [string] $line,
        '^\s*(?<name>.+?):\s*(?<passed>\d+)/(?<total>\d+)\s+checks passed\s*$')
    if ($match.Success) {
        $suites[$match.Groups['name'].Value] = [int] $match.Groups['total'].Value
    }
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json

# 三类结果分开收集：前两类是失败，第三类只提醒。
$missing  = @()
$shrunk   = @()
foreach ($want in $manifest.suites) {
    if (-not $suites.ContainsKey($want.name)) {
        $missing += $want.name
        continue
    }
    if ($suites[$want.name] -lt $want.minAssertions) {
        $shrunk += ('{0}（现在 {1} 条，清单要求至少 {2} 条）' -f
            $want.name, $suites[$want.name], $want.minAssertions)
    }
}
$known = @($manifest.suites | ForEach-Object { $_.name })
$unlisted = @($suites.Keys | Where-Object { $known -notcontains $_ })

Write-Host ('套件清单核验：输出里有 {0} 个套件，清单里列了 {1} 个。' -f
    $suites.Count, @($manifest.suites).Count)
foreach ($name in $unlisted) {
    Write-Host ('  [提醒] 清单外的新套件：{0} —— 新增套件是好事，请更新 {1}。' -f
        $name, $ManifestPath)
}

$failed = $false
foreach ($name in $missing) {
    Write-Host ('  [FAIL] 清单里的套件没有出现在输出里：{0}' -f $name)
    $failed = $true
}
foreach ($text in $shrunk) {
    Write-Host ('  [FAIL] 断言数缩水：{0}' -f $text)
    $failed = $true
}
if ($runExit -ne 0) {
    Write-Host ('  [FAIL] 测试进程退出码 {0}' -f $runExit)
    $failed = $true
}

if ($failed) {
    Write-Host 'KSWORD_SUITE_MANIFEST=FAIL'
    exit 1
}
Write-Host 'KSWORD_SUITE_MANIFEST=PASS'
exit 0
