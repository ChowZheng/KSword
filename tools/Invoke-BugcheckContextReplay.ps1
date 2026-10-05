param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outputRoot = Join-Path $repositoryRoot 'output'
$source = Join-Path $PSScriptRoot 'bugcheck_context_replay.c'
$executable = Join-Path $outputRoot 'bugcheck_context_replay.exe'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$kitRoot = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$stubInclude = Join-Path $PSScriptRoot 'bugcheck_layout_replay\stubs'
$kmInclude = Join-Path $kitRoot 'km'
$sharedInclude = Join-Path $kitRoot 'shared'
$ucrtInclude = Join-Path $kitRoot 'ucrt'
foreach ($required in @($vcvars, $kmInclude, $sharedInclude, $ucrtInclude, $stubInclude, $source, $outputRoot)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Required replay input not found: $required" }
}

# Compile real production .c bodies in the replay translation unit with real WDK types.
$compile = @(
    'call', ('"{0}"' -f $vcvars), '>', 'nul', '&&',
    'cl.exe', '/nologo', '/W4', '/WX', '/Od', '/Zi', '/TC', '/utf-8', '/DNDEBUG',
    '/D_AMD64_', '/DAMD64', '/D_WIN32_WINNT=0x0A00',
    ('/I"{0}"' -f $stubInclude), ('/I"{0}"' -f $kmInclude),
    ('/I"{0}"' -f $sharedInclude), ('/I"{0}"' -f $ucrtInclude),
    ('/Fo"{0}"' -f (Join-Path $outputRoot 'bugcheck_context_replay.obj')),
    ('/Fd"{0}"' -f (Join-Path $outputRoot 'bugcheck_context_replay_compile.pdb')),
    ('/Fe"{0}"' -f $executable), ('"{0}"' -f $source),
    '/link', 'kernel32.lib', '/INCREMENTAL:NO',
    ('/PDB:"{0}"' -f (Join-Path $outputRoot 'bugcheck_context_replay.pdb'))
) -join ' '
cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $executable
exit $LASTEXITCODE
