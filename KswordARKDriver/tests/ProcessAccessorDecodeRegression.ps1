[CmdletBinding()]
param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..'))
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
# Use the existing repository output directory; do not create a build checkout.
$taskOutput = Join-Path $taskRoot 'output'
if (!(Test-Path -LiteralPath $taskOutput -PathType Container)) { throw 'Existing output directory is required.' }
$taskVcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) { $taskVcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat' }
if (!(Test-Path -LiteralPath $taskVcvars)) { throw '64-bit MSVC environment is required.' }
$taskDecoder = Join-Path $taskRoot 'KswordARKDriver\src\platform\process_accessor_decode.c'
$taskTests = Join-Path $taskRoot 'KswordARKDriver\tests\process_accessor_decode_regression.c'
$taskExecutable = Join-Path $taskOutput 'process_accessor_decode_regression.exe'
$taskCompile = @('call', ('"{0}"' -f $taskVcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/utf-8', '/TC',
    ('/Fe"{0}"' -f $taskExecutable), ('/Fo"{0}/"' -f $taskOutput),
    ('"{0}"' -f $taskDecoder), ('"{0}"' -f $taskTests))
# Compile the unmodified production decoder with the standalone fixture harness.
& $env:ComSpec /d /c ($taskCompile -join ' ')
if ($LASTEXITCODE -ne 0) { throw "Accessor decoder regression compile failed: $LASTEXITCODE" }
& $taskExecutable
if ($LASTEXITCODE -ne 0) { throw "Accessor decoder regression failed: $LASTEXITCODE" }
