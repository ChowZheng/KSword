param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CppCompiler = 'g++'
)
$ErrorActionPreference = 'Stop'
$historyRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$historyOutput = Join-Path $historyRepository '.codex-tmp\memory-history-tests'
$historyCore = Join-Path $historyRepository 'Ksword5.1\Ksword5.1\UI\MemoryEditHistory.Core.cpp'
$historyTests = Join-Path $historyRepository 'tools\memory_edit_history_tests.cpp'
$historyExecutable = Join-Path $historyOutput 'memory_edit_history_tests.exe'
Get-Command $CppCompiler -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $historyOutput -Force | Out-Null
& $CppCompiler -std=c++17 -Wall -Wextra $historyTests $historyCore -o $historyExecutable
if ($LASTEXITCODE -ne 0) { throw 'The memory history test build failed.' }
& $historyExecutable
if ($LASTEXITCODE -ne 0) { throw 'Memory history regression tests failed.' }
