param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CppCompiler = 'g++'
)
$ErrorActionPreference = 'Stop'
$pointerRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$pointerOutput = Join-Path $pointerRepository '.codex-tmp\pointer-chain-tests'
$pointerCore = Join-Path $pointerRepository 'shared\evidence\PointerChain.cpp'
$pointerTests = Join-Path $pointerRepository 'tools\pointer_chain_tests.cpp'
$pointerExecutable = Join-Path $pointerOutput 'pointer_chain_tests.exe'
Get-Command $CppCompiler -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $pointerOutput -Force | Out-Null
if ([System.IO.Path]::GetFileName($CppCompiler) -match '^cl(\.exe)?$') {
    Push-Location -LiteralPath $pointerOutput
    try { & $CppCompiler /nologo /std:c++17 /EHsc /W4 /utf-8 /O2 $pointerTests $pointerCore "/Fe:$pointerExecutable" }
    finally { Pop-Location }
} else {
    & $CppCompiler -std=c++17 -Wall -Wextra -Werror $pointerTests $pointerCore -o $pointerExecutable
}
if ($LASTEXITCODE -ne 0) { throw 'The pointer-chain test build failed.' }
& $pointerExecutable
if ($LASTEXITCODE -ne 0) { throw 'Pointer-chain regression tests failed.' }
