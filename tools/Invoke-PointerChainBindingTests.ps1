param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CppCompiler = 'g++'
)
$ErrorActionPreference = 'Stop'
$bindingRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$bindingOutput = Join-Path $bindingRepository '.codex-tmp\pointer-chain-tests'
$bindingCore = Join-Path $bindingRepository 'shared\evidence\memory_workbench'
$bindingExecutable = Join-Path $bindingOutput 'pointer_chain_binding_tests.exe'
Get-Command $CppCompiler -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $bindingOutput -Force | Out-Null
$bindingSources = @(
    (Join-Path $bindingRepository 'tools\pointer_chain_binding_tests.cpp'),
    (Join-Path $bindingRepository 'shared\evidence\PointerChain.cpp'),
    (Join-Path $bindingCore 'MemoryAddressBook.cpp'),
    (Join-Path $bindingCore 'MemoryTargetSession.cpp'),
    (Join-Path $bindingCore 'PointerChainBindings.cpp')
)
if ([System.IO.Path]::GetFileName($CppCompiler) -match '^cl(\.exe)?$') {
    Push-Location -LiteralPath $bindingOutput
    try { & $CppCompiler /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 @bindingSources "/Fe:$bindingExecutable" }
    finally { Pop-Location }
} else {
    & $CppCompiler -std=c++20 -Wall -Wextra -Werror -O2 @bindingSources -o $bindingExecutable
}
if ($LASTEXITCODE -ne 0) { throw 'The pointer-chain binding test build failed.' }
& $bindingExecutable
if ($LASTEXITCODE -ne 0) { throw 'Pointer-chain binding tests failed.' }
