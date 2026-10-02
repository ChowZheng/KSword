param(
    [string]$Compiler = 'g++',
    [string]$OutputDirectory = '',
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot '.codex-build-logs\syscall-monitor-tests'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$testSource = Join-Path $PSScriptRoot 'etw_probe.cpp'
$testBinary = Join-Path $OutputDirectory 'syscall-etw-probe.exe'
$compilerCommand = (Get-Command $Compiler -ErrorAction Stop).Source

# Probe runtime uses only this process, normal Windows exports, and its own
# newly created ETW session. This script never elevates or retries as admin.
& $compilerCommand -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -static -static-libgcc -static-libstdc++ $testSource -o $testBinary -ladvapi32 -lole32
if ($LASTEXITCODE -ne 0) { throw "Syscall ETW probe compilation failed ($LASTEXITCODE)." }
Write-Output 'SYSCALL_ETW_PROBE_BUILD=PASS'
if ($BuildOnly) {
    Write-Output 'SYSCALL_ETW_PROBE=CAPTURE_NOT_VERIFIED'
    Write-Output 'REASON=BUILD_ONLY'
    return
}
& $testBinary
$probeExit = $LASTEXITCODE
if ($probeExit -ne 0) {
    Write-Output "SYSCALL_ETW_PROBE_EXIT_CODE=$probeExit"
}
exit $probeExit
