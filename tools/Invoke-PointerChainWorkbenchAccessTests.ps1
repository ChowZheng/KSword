param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CppCompiler = 'g++'
)
$ErrorActionPreference = 'Stop'
$pointerAccessRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$pointerAccessOutput = Join-Path $pointerAccessRepository '.codex-tmp\pointer-workbench-tests'
$pointerAccessExecutable = Join-Path $pointerAccessOutput 'pointer_chain_workbench_access_tests.exe'
$pointerAccessSources = @(
    'tools\pointer_chain_workbench_access_tests.cpp',
    'Ksword5.1\Ksword5.1\MemoryDock\WorkbenchPointerChainAccess.cpp',
    'shared\evidence\PointerChain.cpp',
    'shared\evidence\memory_workbench\MemoryTargetSession.cpp',
    'shared\evidence\memory_workbench\MemoryAddressBook.cpp',
    'shared\evidence\memory_workbench\MemoryAddressBook.Serialize.cpp',
    'shared\evidence\memory_workbench\SessionAddressResolver.cpp',
    'shared\evidence\memory_workbench\MemoryAddressExpr.cpp',
    'shared\evidence\memory_workbench\MemoryModuleDirectory.cpp'
) | ForEach-Object { Join-Path $pointerAccessRepository $_ }
Get-Command $CppCompiler -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $pointerAccessOutput -Force | Out-Null
$pointerAccessCompilerName = Split-Path -Leaf $CppCompiler
if ($pointerAccessCompilerName -match '^cl(\.exe)?$') {
    Push-Location -LiteralPath $pointerAccessOutput
    try {
        & $CppCompiler /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 @pointerAccessSources "/Fe:$pointerAccessExecutable" /link Psapi.lib
        $pointerAccessBuildExit = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
}
else {
    & $CppCompiler -std=c++20 -Wall -Wextra -Werror @pointerAccessSources -lpsapi -o $pointerAccessExecutable
    $pointerAccessBuildExit = $LASTEXITCODE
}
if ($pointerAccessBuildExit -ne 0) { throw 'Pointer workbench native metadata test compilation failed.' }
& $pointerAccessExecutable
if ($LASTEXITCODE -ne 0) { throw 'Pointer workbench native metadata regression tests failed.' }
