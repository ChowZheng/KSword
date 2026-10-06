# 在既有日志目录运行生产配色/绘制夹具，不启动硬件采样、安装、UAC或驱动业务。
param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$colorRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$colorOutput = Join-Path $colorRepository '.codex-build-logs'
if (!(Test-Path -LiteralPath $colorOutput)) { throw 'Existing log directory is required.' }
$colorQt = Join-Path $colorRepository '.deps\Qt\6.9.3\msvc2022_64'
$colorVc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$colorSdk = 'C:\Program Files (x86)\Windows Kits\10'
$colorSdkVersion = '10.0.26100.0'
$colorCompiler = Join-Path $colorVc 'bin\HostX64\x64\cl.exe'
$colorIncludes = @('/I' + (Join-Path $colorVc 'include'))
foreach ($colorModule in @('', 'QtCore', 'QtGui', 'QtWidgets', 'QtConcurrent')) {
    $colorIncludes += '/external:I' + (Join-Path $colorQt ('include\' + $colorModule))
}
foreach ($colorPart in @('ucrt', 'shared', 'um')) {
    $colorIncludes += '/external:I' + (Join-Path $colorSdk ('Include\' + $colorSdkVersion + '\' + $colorPart))
}
$colorLibraries = @(('/LIBPATH:' + (Join-Path $colorQt 'lib')), ('/LIBPATH:' + (Join-Path $colorVc 'lib\x64')))
foreach ($colorPart in @('ucrt', 'um')) {
    $colorLibraries += '/LIBPATH:' + (Join-Path $colorSdk ('Lib\' + $colorSdkVersion + '\' + $colorPart + '\x64'))
}

# 链接本轮标准 HUD Build 的真实生产对象；不重建或替换生产项目的目录和 TargetName。
$colorObjects = @('HudProcessListPanel.obj', 'HudPerformancePanel.obj', 'PerformanceNavCard.obj', 'KsPainterChart.obj', 'Get.obj') |
    ForEach-Object { Join-Path $colorRepository ('KswordHUD\KswordHUD\x64\Release\' + $_) }
foreach ($colorObject in $colorObjects) {
    if (!(Test-Path -LiteralPath $colorObject)) { throw "Required production object is missing: $colorObject" }
}
$colorExe = Join-Path $colorOutput 'other_gui_color_tests.exe'
& $colorCompiler /nologo /std:c++17 /Zc:__cplusplus /permissive- /utf-8 /EHsc /MD /W4 /WX /O2 /external:W0 `
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE @colorIncludes `
    (Join-Path $colorRepository 'tools\other_gui_color_tests.cpp') @colorObjects `
    ('/Fo' + (Join-Path $colorOutput 'other_gui_color_tests.obj')) ('/Fe' + $colorExe) `
    /link /LTCG /OPT:REF /SUBSYSTEM:CONSOLE /INCREMENTAL:NO @colorLibraries `
    Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Qt6Concurrent.lib user32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib
if ($LASTEXITCODE -ne 0) { throw 'HUD color fixture compilation/link failed.' }

# Qt离屏执行只创建无采样卡片、隐藏进程页和纯图表，不显示生产 HUD。
$savedColorPath = $env:PATH
$savedColorPlatform = $env:QT_QPA_PLATFORM
$savedColorPlugins = $env:QT_PLUGIN_PATH
try {
    $env:PATH = (Join-Path $colorQt 'bin') + ';' + $savedColorPath
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_PLUGIN_PATH = Join-Path $colorQt 'plugins'
    & $colorExe
    if ($LASTEXITCODE -ne 0) { throw 'HUD color behavior regression failed.' }
}
finally {
    $env:PATH = $savedColorPath
    $env:QT_QPA_PLATFORM = $savedColorPlatform
    $env:QT_PLUGIN_PATH = $savedColorPlugins
}

# Setup使用标准构建的KTheme生产对象；只调用纯颜色函数，没有安装器入口。
$setupIncludes = $colorIncludes + @('/external:I' + (Join-Path $colorRepository 'KswordSetup\fltk\include'))
$setupObject = Join-Path $colorRepository 'KswordSetup\x64\Release\obj\KTheme.obj'
if (!(Test-Path -LiteralPath $setupObject)) { throw 'Build standard Setup before running its color regression.' }
$setupImageObject = Join-Path $colorRepository 'KswordSetup\x64\Release\obj\KImage.obj'
$setupWidgetsObject = Join-Path $colorRepository 'KswordSetup\x64\Release\obj\KWidgets.obj'
$setupExe = Join-Path $colorOutput 'setup_color_tests.exe'
& $colorCompiler /nologo /std:c++17 /utf-8 /EHsc /MT /W4 /WX /O2 /external:W0 @setupIncludes `
    (Join-Path $colorRepository 'tools\setup_color_tests.cpp') $setupObject $setupImageObject $setupWidgetsObject `
    ('/Fo' + (Join-Path $colorOutput 'setup_color_tests.obj')) ('/Fe' + $setupExe) `
    /link /LTCG /OPT:REF /SUBSYSTEM:CONSOLE /INCREMENTAL:NO @colorLibraries `
    ('/LIBPATH:' + (Join-Path $colorRepository 'KswordSetup\fltk\Release\lib')) `
    fltk.lib fltk_images.lib fltk_jpeg.lib fltk_png.lib fltk_z.lib user32.lib gdi32.lib shell32.lib ole32.lib `
    comctl32.lib advapi32.lib gdiplus.lib wsock32.lib winspool.lib comdlg32.lib uuid.lib
if ($LASTEXITCODE -ne 0) { throw 'Setup color fixture compilation/link failed.' }
& $setupExe
if ($LASTEXITCODE -ne 0) { throw 'Setup color behavior regression failed.' }
