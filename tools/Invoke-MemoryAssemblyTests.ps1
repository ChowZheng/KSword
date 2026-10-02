param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CCompiler = 'gcc',
    [string]$CppCompiler = 'g++'
)

$ErrorActionPreference = 'Stop'
$assemblyRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$assemblyTestOutput = Join-Path $assemblyRepository '.codex-tmp\memory-assembly-tests'
$assemblyZydis = Join-Path $assemblyRepository 'third_party\zydis'
$assemblyCore = Join-Path $assemblyRepository 'Ksword5.1\Ksword5.1\UI\MemoryAssembly.Core.cpp'
$assemblyTests = Join-Path $assemblyRepository 'tools\memory_assembly_tests.cpp'
$assemblyObject = Join-Path $assemblyTestOutput 'Zydis.o'
$assemblyTestExe = Join-Path $assemblyTestOutput 'memory_assembly_tests.exe'

Get-Command $CCompiler -ErrorAction Stop | Out-Null
Get-Command $CppCompiler -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $assemblyTestOutput -Force | Out-Null

& $CCompiler -std=c11 -O0 -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I $assemblyZydis `
    -c (Join-Path $assemblyZydis 'Zydis.c') -o $assemblyObject
if ($LASTEXITCODE -ne 0) { throw 'The bundled Zydis test build failed.' }

& $CppCompiler -std=c++17 -Wall -Wextra -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD `
    -I $assemblyZydis $assemblyTests $assemblyCore $assemblyObject -o $assemblyTestExe
if ($LASTEXITCODE -ne 0) { throw 'The memory assembly test build failed.' }

& $assemblyTestExe
if ($LASTEXITCODE -ne 0) { throw 'Memory assembly regression tests failed.' }
