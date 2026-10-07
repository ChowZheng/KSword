param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$CppCompiler = 'g++'
)
$ErrorActionPreference = 'Stop'
$pageReaderRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$pageReaderOutput = Join-Path $pageReaderRepository '.codex-tmp\memory-page-reader-tests'
$pageReaderExecutable = Join-Path $pageReaderOutput 'memory_page_reader_win32_tests.exe'
Get-Command $CppCompiler -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $pageReaderOutput -Force | Out-Null
$pageReaderSources = @(
    (Join-Path $pageReaderRepository 'tools\memory_page_reader_win32_tests.cpp'),
    (Join-Path $pageReaderRepository 'KswordARKLightTests\MemoryPageReaderTests.cpp'),
    (Join-Path $pageReaderRepository 'shared\evidence\memory_workbench\MemoryPageReader.cpp')
)
if ([System.IO.Path]::GetFileName($CppCompiler) -match '^cl(\.exe)?$') {
    Push-Location -LiteralPath $pageReaderOutput
    try { & $CppCompiler /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 @pageReaderSources "/Fe:$pageReaderExecutable" }
    finally { Pop-Location }
} else {
    & $CppCompiler -std=c++20 -Wall -Wextra -Werror -O2 @pageReaderSources -o $pageReaderExecutable
}
if ($LASTEXITCODE -ne 0) { throw 'The MemoryPageReader Win32 test build failed.' }
& $pageReaderExecutable
if ($LASTEXITCODE -ne 0) { throw 'MemoryPageReader Win32 tests failed.' }
