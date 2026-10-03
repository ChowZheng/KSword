[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..'))
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$taskOutput = Join-Path $taskRoot '.codex-build-logs\file-delete-irp'
$taskVcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) { $taskVcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat' }
$taskKit = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
foreach ($taskRequired in @($taskVcvars, $taskKit)) {
    if (!(Test-Path -LiteralPath $taskRequired)) { throw "Required input missing: $taskRequired" }
}
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskSource = Join-Path $taskRoot 'KswordARKDriver\tests\file_delete_irp_regression.c'
$taskExecutable = Join-Path $taskOutput 'file_delete_irp_regression.exe'
$taskObject = Join-Path $taskOutput 'file_delete_irp_regression.obj'
$taskIncludes = @((Join-Path $taskKit 'km'), (Join-Path $taskKit 'shared'), (Join-Path $taskKit 'ucrt'),
    (Join-Path $taskRoot 'KswordARKDriver\include'), (Join-Path $taskRoot 'shared'))
$taskCompile = @('call', ('"{0}"' -f $taskVcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/utf-8', '/D_AMD64_', '/D_WIN64',
    ('/Fe"{0}"' -f $taskExecutable), ('/Fo"{0}"' -f $taskObject))
$taskCompile += $taskIncludes | ForEach-Object { '/I"{0}"' -f $_ }
$taskCompile += '"{0}"' -f $taskSource
& $env:ComSpec /d /c ($taskCompile -join ' ')
if ($LASTEXITCODE -ne 0) { throw "Regression compile failed: $LASTEXITCODE" }
& $taskExecutable
if ($LASTEXITCODE -ne 0) { throw "File delete IRP regression failed: $LASTEXITCODE" }
