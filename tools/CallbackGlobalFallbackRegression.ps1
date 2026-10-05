param()

# 中文说明：仅运行用户态离线模拟，直接编译生产回退 helper；不会读取内核、注册回调或加载驱动。
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outputRoot = Join-Path $repositoryRoot 'output'
$source = Join-Path $PSScriptRoot 'callback_global_fallback_tests.c'
$executable = Join-Path $outputRoot 'callback_global_fallback_tests.exe'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) {
    # 中文说明：仅按已知安装约定路径回退，不递归搜索磁盘。
    $vcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat'
}
$kitRoot = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$stubInclude = Join-Path $PSScriptRoot 'callback_global_fallback_stubs'
foreach ($required in @($vcvars, $kitRoot, $stubInclude, $source, $outputRoot)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Required replay input not found: $required" }
}

# 中文说明：使用真实 WDK 类型，只有 OS 服务由测试替换，生产判据原样编译。
# 输出固定在已有 output/，OBJ、EXE 和编译器 PDB 各自命名，避免并行回归共享默认 PDB。
$compile = @(
    'call', ('"{0}"' -f $vcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/Od', '/TC', '/utf-8',
    '/D_AMD64_', '/DAMD64', '/D_WIN32_WINNT=0x0A00',
    ('/I"{0}"' -f $stubInclude), ('/I"{0}\km"' -f $kitRoot),
    ('/I"{0}\shared"' -f $kitRoot), ('/I"{0}\ucrt"' -f $kitRoot),
    ('/Fo"{0}"' -f (Join-Path $outputRoot 'callback_global_fallback_tests.obj')),
    ('/Fd"{0}"' -f (Join-Path $outputRoot 'callback_global_fallback_tests_compile.pdb')),
    ('/Fe"{0}"' -f $executable), ('"{0}"' -f $source),
    '/link', 'kernel32.lib', '/INCREMENTAL:NO'
) -join ' '
cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $executable
exit $LASTEXITCODE
