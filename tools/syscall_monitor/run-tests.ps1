param(
    [string]$Compiler = 'g++',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot '.codex-build-logs\syscall-monitor-tests'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$compilerCommand = (Get-Command $Compiler -ErrorAction Stop).Source

foreach ($suite in @('evidence', 'correlation')) {
    $testSource = Join-Path $PSScriptRoot ($suite + '_tests.cpp')
    $testBinary = Join-Path $OutputDirectory ('syscall-' + $suite + '-tests.exe')
    & $compilerCommand -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -static -static-libgcc -static-libstdc++ $testSource -o $testBinary
    if ($LASTEXITCODE -ne 0) { throw "Syscall $suite test compilation failed ($LASTEXITCODE)." }
    & $testBinary
    if ($LASTEXITCODE -ne 0) { throw "Syscall $suite regressions failed ($LASTEXITCODE)." }
}
