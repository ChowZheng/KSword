[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..'))

# 中文说明：从真实扫描器原样提取实现，在已有 output 中编译合成 PE 回归，不装载驱动。
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$taskOutput = Join-Path $taskRoot 'output'
$taskVcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) { $taskVcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat' }
$taskKit = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
foreach ($taskRequired in @($taskOutput, $taskVcvars, $taskKit)) {
    if (!(Test-Path -LiteralPath $taskRequired)) { throw "Required input missing: $taskRequired" }
}
# 中文说明：移除仅供内核链接的导出声明，函数体不改写；头文件仍来自生产代码。
$taskProduction = [IO.File]::ReadAllText((Join-Path $taskRoot 'KswordARKDriver\src\platform\runtime_signature_scan.c'))
$taskStart = $taskProduction.IndexOf('typedef struct _KSW_RUNTIME_ROUTINE_WORK')
if ($taskStart -lt 0) { throw 'Cannot locate production scanner definitions.' }
$taskExtracted = Join-Path $taskOutput 'runtime_signature_regression_source.c'
[IO.File]::WriteAllText($taskExtracted, $taskProduction.Substring($taskStart), [Text.UTF8Encoding]::new($false))
$taskExecutable = Join-Path $taskOutput 'runtime_signature_regression.exe'
$taskSource = Join-Path $taskRoot 'KswordARKDriver\tests\runtime_signature_regression.c'
$taskIncludes = @((Join-Path $taskKit 'km'), (Join-Path $taskKit 'shared'), (Join-Path $taskKit 'ucrt'),
    (Join-Path $taskRoot 'KswordARKDriver\src\platform'))
# 中文说明：每套测试使用独立 obj/pdb/exe，支持与其它任务的离线编译并发。
$taskCompile = @('call', ('"{0}"' -f $taskVcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/Od', '/Gy', '/utf-8', '/TC', '/D_AMD64_', '/DAMD64')
$taskCompile += $taskIncludes | ForEach-Object { '/I"{0}"' -f $_ }
$taskCompile += @(('/Fo"{0}"' -f (Join-Path $taskOutput 'runtime_signature_regression.obj')),
    ('/Fd"{0}"' -f (Join-Path $taskOutput 'runtime_signature_regression_compile.pdb')),
    ('/Fe"{0}"' -f $taskExecutable), ('"{0}"' -f $taskSource), '/link', '/OPT:REF')
& cmd.exe /d /s /c ($taskCompile -join ' ')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskExecutable
exit $LASTEXITCODE
