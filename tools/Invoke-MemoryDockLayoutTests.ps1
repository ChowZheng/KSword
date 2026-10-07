# 内存 Dock 页面布局层 3 回归：链接真实 MemoryDock 及其全部生产依赖，离屏运行 tools\memory_dock_layout_tests.cpp。
#
# 前提（必读）：需要先有一次完整的 Release 主程序构建——本脚本读取 Release 构建遗留的
#   link.command.1.tlog，取出全部生产 .obj（排除 main.obj）去链接测试入口，做法同
#   tools\Invoke-MemoryEditorUiTests.ps1。没有 Release 构建时脚本直接报错，不会去编译整套生产源码。
#
# 用法：
#   powershell -File tools\Invoke-MemoryDockLayoutTests.ps1                  编译、链接并运行（断言模式，失败退出码非零）
#   powershell -File tools\Invoke-MemoryDockLayoutTests.ps1 -Baseline        运行基线模式：只打印每页最小提示与各项失败行，退出码恒为 0
#   powershell -File tools\Invoke-MemoryDockLayoutTests.ps1 -SkipRun         只编译链接，不运行
#
# 对比基线的方法：
#   1) 在修复前的 Release 构建上跑 -Baseline：应看到至少一页最小高度 > 1000、dock 最小高度 > 1000，且 shell=0；
#   2) 在修复后的 Release 构建上跑（不带 -Baseline）：每页接近 0、shell=1（工作台页除外）、全部断言通过。
param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$Baseline,
    [switch]$SkipRun
)
$ErrorActionPreference = 'Stop'
$testRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$testOutput = Join-Path $testRepository '.codex-build-logs'
if (!(Test-Path -LiteralPath $testOutput)) { New-Item -ItemType Directory -Path $testOutput | Out-Null }

# testQt：优先仓库随附的 Qt，其次备用安装位置。
$testQt = Join-Path $testRepository '.deps\Qt\6.9.3\msvc2022_64'
if (!(Test-Path -LiteralPath $testQt)) { $testQt = 'D:\Software\Qt\6.9.3\msvc2022_64' }
if (!(Test-Path -LiteralPath (Join-Path $testQt 'lib\Qt6Widgets.lib'))) { throw 'Qt 6.9.3 msvc2022_64 was not found.' }

# testVcRoot：与既有测试脚本同一个 MSVC 工具集目录；该版本不存在时退回 MSVC 目录下最新的一个（只列这一层目录，不扫盘）。
$testVcBase = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$testVcRoot = Join-Path $testVcBase '14.44.35207'
if (!(Test-Path -LiteralPath $testVcRoot)) {
    $testVcCandidate = Get-ChildItem -LiteralPath $testVcBase -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if (!$testVcCandidate) { throw 'No MSVC toolset was found under the BuildTools directory.' }
    $testVcRoot = $testVcCandidate.FullName
}
$testSdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$testSdkVersion = '10.0.26100.0'
$testCompiler = Join-Path $testVcRoot 'bin\Hostx64\x64\cl.exe'
$testLinker = Join-Path $testVcRoot 'bin\Hostx64\x64\link.exe'
$testSource = Join-Path $testRepository 'tools\memory_dock_layout_tests.cpp'
$testObject = Join-Path $testOutput 'memory_dock_layout_tests.obj'
$testExe = Join-Path $testOutput 'memory_dock_layout_tests.exe'

# testIncludeArgs：与生产工程同一组包含目录（工程目录、zydis、zstd、Qt 各模块）；第三方头一律走 external，避免它们的警告挡住测试。
$testIncludeArgs = @(
    ('/I' + (Join-Path $testRepository 'Ksword5.1\Ksword5.1')),
    ('/I' + (Join-Path $testRepository 'third_party\zydis')),
    ('/I' + (Join-Path $testRepository 'third_party\zstd')),
    ('/external:I' + (Join-Path $testVcRoot 'include')),
    ('/external:I' + (Join-Path $testVcRoot 'atlmfc\include'))
)
foreach ($testModule in @('', 'QtCore', 'QtGui', 'QtWidgets', 'QtNetwork', 'QtSvg')) {
    $testIncludeArgs += '/external:I' + (Join-Path $testQt ('include\' + $testModule))
}
foreach ($testSdkPart in @('ucrt', 'shared', 'um')) {
    $testIncludeArgs += '/external:I' + (Join-Path $testSdkRoot ('Include\' + $testSdkVersion + '\' + $testSdkPart))
}

# 编译测试入口。项目头文件不是 /W4 干净的，所以这里用 /W3 且不开 /WX（与 Invoke-ThemeColorComponentTests.ps1 一致）。
& $testCompiler /nologo /std:c++latest /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W3 /wd4702 /O2 /Gy /bigobj /external:W0 `
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /DZYDIS_STATIC_BUILD `
    /DQT_WIDGETS_LIB /DQT_GUI_LIB /DQT_CORE_LIB @testIncludeArgs /c $testSource ('/Fo' + $testObject)
if ($LASTEXITCODE -ne 0) { throw 'Memory dock layout test compilation failed.' }

# 读取已有 Release 构建的链接记录：第一行是全部输入对象，第二行是完整链接选项。
$testLinkCandidates = @(
    'Ksword5.1\Ksword5.1\x64\Release\Ksword5.1.tlog\link.command.1.tlog',
    'Ksword5.1\Ksword5.1\Ksword5.1\x64\Release\Ksword5.1.tlog\link.command.1.tlog'
) | ForEach-Object { Get-Item -LiteralPath (Join-Path $testRepository $_) -ErrorAction SilentlyContinue }
if (!$testLinkCandidates) {
    throw 'No Release link record was found. Build the main program in Release first (this test links the real production objects).'
}
$testLinkLog = ($testLinkCandidates | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
$testLinkLines = Get-Content -LiteralPath $testLinkLog
$testInputs = @()
$testLinkFlags = $null
for ($testLineIndex = 1; $testLineIndex -lt $testLinkLines.Count; ++$testLineIndex) {
    if ($testLinkLines[$testLineIndex] -match '/OUT:"[^"\r\n]*\\KSWORD5\.1\.EXE"') {
        $testInputs = @($testLinkLines[$testLineIndex - 1].TrimStart('^').Split('|'))
        $testLinkFlags = $testLinkLines[$testLineIndex]
    }
}
if (!$testLinkFlags -or $testInputs.Count -lt 2) { throw 'Production link record was not found.' }

# 确认链接记录里确实有 MemoryDock 的对象：旧的/不完整的构建里没有它，链接会给出莫名其妙的未解析符号。
if (!($testInputs | Where-Object { $_ -match 'MEMORYDOCK\.UIBUILD\.OBJ$' })) {
    throw 'The selected production link record does not contain MemoryDock.UiBuild.obj.'
}
$testMainObjects = @($testInputs | Where-Object { [IO.Path]::GetFileName($_) -ieq 'main.obj' })
if ($testMainObjects.Count -ne 1) { throw 'Production entry point is ambiguous.' }
# 主程序现在把 Zydis/zstd 作为静态库工程（KswordZydis.lib / KswordZstd.lib）链接，它们在链接记录里是 .LIB 输入；
# 只留 .OBJ 会丢掉它们，链接报 Zydis*/ZSTD_* 未解析。所以 .LIB 输入一并保留（与 Qt 库重复无害）。
$testInputs = @($testInputs | Where-Object { $_ -notin $testMainObjects -and $_ -match '\.(OBJ|LIB)$' })

# testResponse：移除原产物路径和 Windows 入口；保留生产库、LTCG 与依赖，再追加测试入口与 ADS 导入库（重复追加无害）。
$testResponse = [regex]::Replace($testLinkFlags, '/(?:OUT|PDB|IMPLIB|LTCGOUT):(?:"[^"]*"|\S+)', '', 'IgnoreCase')
$testResponse = [regex]::Replace($testResponse, '/SUBSYSTEM:\S+|\S+\.RES\b', '', 'IgnoreCase')
$testResponse += ' /OUT:"' + $testExe + '" /SUBSYSTEM:CONSOLE /INCREMENTAL:NO "' + $testObject + '"'
$testResponse += ' /LTCG:INCREMENTAL /LTCGOUT:"' + (Join-Path $testOutput 'memory_dock_layout_tests.iobj') + '"'
$testResponse += ' qtadvanceddocking.lib'
$testResponse += ' /LIBPATH:"' + (Join-Path $testVcRoot 'lib\x64') + '"'
$testResponse += ' /LIBPATH:"' + (Join-Path $testVcRoot 'atlmfc\lib\x64') + '"'
$testResponse += ' /LIBPATH:"' + (Join-Path $testRepository 'Ksword5.1\Ksword5.1\lib') + '"'
foreach ($testSdkPart in @('ucrt', 'um')) {
    $testResponse += ' /LIBPATH:"' + (Join-Path $testSdkRoot ('Lib\' + $testSdkVersion + '\' + $testSdkPart + '\x64')) + '"'
}
foreach ($testInput in $testInputs) { $testResponse += "`r`n`"" + $testInput + '"' }
$testResponseFile = Join-Path $testOutput 'memory-dock-layout-tests.rsp'
[IO.File]::WriteAllText($testResponseFile, $testResponse, [Text.Encoding]::Unicode)
$testLinkPreviousPath = $env:PATH
try {
    # 清单嵌入调用 rc/mt，必须提供同一 SDK 的 x64 工具目录。
    $env:PATH = (Join-Path $testSdkRoot ('bin\' + $testSdkVersion + '\x64')) + ';' + $testLinkPreviousPath
    & $testLinker ('@' + $testResponseFile)
    $testLinkExit = $LASTEXITCODE
}
finally { $env:PATH = $testLinkPreviousPath }
if ($testLinkExit -ne 0) { throw 'Memory dock layout test link failed.' }

if ($SkipRun) { return }

# 离屏运行：测试只使用重定向后的 INI 设置；Qt/ADS DLL 从已构建 Release 目录定位。
$testRunArgs = @()
if ($Baseline) { $testRunArgs += '--baseline' }
$testOldPath = $env:PATH
$testOldPlatform = $env:QT_QPA_PLATFORM
$testOldPluginPath = $env:QT_PLUGIN_PATH
try {
    $env:PATH = (Join-Path $testRepository 'Ksword5.1\x64\Release') + ';' + (Join-Path $testQt 'bin') + ';' + $testOldPath
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_PLUGIN_PATH = Join-Path $testQt 'plugins'
    & $testExe @testRunArgs
    if ($LASTEXITCODE -ne 0) { throw 'Memory dock layout regression failed.' }
}
finally {
    $env:PATH = $testOldPath
    $env:QT_QPA_PLATFORM = $testOldPlatform
    $env:QT_PLUGIN_PATH = $testOldPluginPath
}
