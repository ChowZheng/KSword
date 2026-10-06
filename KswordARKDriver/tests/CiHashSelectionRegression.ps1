[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot '..\..')
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$taskOutput = Join-Path $taskRoot 'output'
if (!(Test-Path -LiteralPath $taskOutput -PathType Container)) {
    throw 'The repository output directory must already exist.'
}
$taskVcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $taskVcvars)) {
    $taskVcvars = 'D:\Software\VS\VC\Auxiliary\Build\vcvars64.bat'
}
if (!(Test-Path -LiteralPath $taskVcvars)) { throw 'x64 MSVC vcvars64.bat is required.' }

$taskProductionPath = Join-Path $taskRoot 'KswordARKDriver\src\features\kernel\ci_hash_fallback.c'
$taskProduction = [IO.File]::ReadAllText($taskProductionPath)
$taskReferenceHeader = [IO.File]::ReadAllText((Join-Path $taskRoot 'KswordARKDriver\src\platform\runtime_signature_scan.h'))
$taskPieces = @(
    [regex]::Match($taskProduction, '(?m)^#define KSW_CI_HASH_MAX_SAMPLES [^\r\n]+'),
    [regex]::Match($taskReferenceHeader, '(?ms)^typedef struct _KSW_RUNTIME_DATA_REFERENCE\b.*?^} KSW_RUNTIME_DATA_REFERENCE, \*PKSW_RUNTIME_DATA_REFERENCE;'),
    [regex]::Match($taskProduction, '(?ms)^typedef struct _KSW_CI_HASH_CANDIDATE\b.*?^} KSW_CI_HASH_CANDIDATE, \*PKSW_CI_HASH_CANDIDATE;'),
    [regex]::Match($taskProduction, '(?ms)^static VOID\r?\nKswordARKCiHashSelectListCandidate\(.*?^}'),
    [regex]::Match($taskProduction, '(?ms)^static VOID\r?\nKswordARKCiHashSelectLockReference\(.*?^}')
)
if ($taskPieces.Where({ !$_.Success }).Count -ne 0) {
    throw 'Cannot locate the exact production candidate types and selection helpers.'
}
# Replay the exact production bodies. No duplicated selection algorithm is kept in the fixture.
$taskReplayHeader = Join-Path $taskOutput 'ci_hash_selection_production.h'
$taskReplayText = ($taskPieces | ForEach-Object { $_.Value }) -join "`r`n`r`n"
[IO.File]::WriteAllText($taskReplayHeader, $taskReplayText + "`r`n", [Text.UTF8Encoding]::new($false))

$taskSource = Join-Path $PSScriptRoot 'ci_hash_selection_regression.c'
$taskExecutable = Join-Path $taskOutput 'ci_hash_selection_regression.exe'
$taskObject = Join-Path $taskOutput 'ci_hash_selection_regression.obj'
$taskCompilePdb = Join-Path $taskOutput 'ci_hash_selection_regression_compile.pdb'
$taskLinkPdb = Join-Path $taskOutput 'ci_hash_selection_regression.pdb'
$taskCompile = @('call', ('"{0}"' -f $taskVcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/Od', '/Zi', '/utf-8', '/TC',
    ('/I"{0}"' -f $taskOutput), ('/Fo"{0}"' -f $taskObject),
    ('/Fd"{0}"' -f $taskCompilePdb), ('/Fe"{0}"' -f $taskExecutable),
    ('"{0}"' -f $taskSource), '/link', '/INCREMENTAL:NO', ('/PDB:"{0}"' -f $taskLinkPdb))
& cmd.exe /d /s /c ($taskCompile -join ' ')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $taskExecutable
exit $LASTEXITCODE
