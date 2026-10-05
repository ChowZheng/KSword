# 编译两份真实Win32日志页并运行隐藏窗口夹具，输出限定既有构建日志目录。
param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$testRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$testOutput = Join-Path $testRoot '.codex-build-logs'
if (!(Test-Path -LiteralPath $testOutput)) { throw 'Existing build log directory is required.' }
$vcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.26100.0'
$compiler = Join-Path $vcRoot 'bin\Hostx64\x64\cl.exe'
$testExe = Join-Path $testOutput 'plugin_theme_ui_tests.exe'
# 夹具直接链接生产LogSurface，禁用Launcher初始化，不启动任何调试器/后端。
$sources = @('tools\plugin_theme_ui_tests.cpp', 'CheatEngineExecutablePlugin\LogSurface.cpp', 'X96dbgExecutablePlugin\LogSurface.cpp') |
    ForEach-Object { Join-Path $testRoot $_ }
$includes = @('/I' + (Join-Path $vcRoot 'include'))
foreach ($part in @('ucrt','shared','um')) {
    $includes += '/external:I' + (Join-Path $sdkRoot ('Include\' + $sdkVersion + '\' + $part))
}
$libraries = @('/LIBPATH:' + (Join-Path $vcRoot 'lib\x64'))
foreach ($part in @('ucrt','um')) {
    $libraries += '/LIBPATH:' + (Join-Path $sdkRoot ('Lib\' + $sdkVersion + '\' + $part + '\x64'))
}
# 两份生产文件都叫LogSurface.cpp，显式使用不同obj名，避免相互覆盖。
$objects = @('plugin_theme_fixture.obj','plugin_theme_ce.obj','plugin_theme_x96.obj') |
    ForEach-Object { Join-Path $testOutput $_ }
for ($index = 0; $index -lt $sources.Count; ++$index) {
    & $compiler /nologo /c /std:c++17 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /external:W0 `
        /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE @includes $sources[$index] ('/Fo' + $objects[$index])
    if ($LASTEXITCODE -ne 0) { throw 'Plugin theme fixture compilation failed.' }
}
& $compiler /nologo @objects ('/Fe' + $testExe) /link /SUBSYSTEM:CONSOLE /INCREMENTAL:NO @libraries user32.lib gdi32.lib
if ($LASTEXITCODE -ne 0) { throw 'Plugin theme fixture compilation failed.' }
& $testExe
if ($LASTEXITCODE -ne 0) { throw 'Plugin theme fixture failed.' }
