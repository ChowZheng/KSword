[CmdletBinding()]
param(
    [ValidateSet('g++', 'cl')][string]$CppCompiler = 'g++',
    [string]$OutputDirectory = '.codex-tmp/pool-analysis-tests',
    [switch]$AllowNativeEtlSkip
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
Push-Location $repository
try {
    $output = [IO.Path]::GetFullPath((Join-Path $repository $OutputDirectory))
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $core = 'shared/evidence/PoolAllocationAnalysis.cpp'
    foreach ($test in @('pool_allocation_analysis_tests', 'pool_trace_reader_tests')) {
        $source = "tools/tests/$test.cpp"
        if (!(Test-Path -LiteralPath $source)) { throw "Missing test source: $source" }
        $exe = Join-Path $output "$test.exe"
        if ($CppCompiler -eq 'cl') {
            $arguments = @('/nologo', '/std:c++17', '/EHsc', '/O2', '/W4', '/WX', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', $source, $core, "/Fe:$exe", "/Fo:$output\")
            if ($test -eq 'pool_trace_reader_tests') { $arguments += @('/link', 'tdh.lib', 'advapi32.lib', 'dbghelp.lib') }
        } else {
            $arguments = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-DUNICODE', '-D_UNICODE', '-DNOMINMAX', $source, $core, '-o', $exe)
            if ($test -eq 'pool_trace_reader_tests') { $arguments += @('-ltdh', '-ladvapi32', '-ldbghelp') }
        }
        & $CppCompiler @arguments
        if ($LASTEXITCODE -ne 0) { throw "Pool analysis compilation failed: $test" }
        $poolTestOutput = & $exe 2>&1
        $poolTestExit = $LASTEXITCODE
        $poolTestOutput | Write-Output
        if ($poolTestExit -ne 0) { throw "Pool analysis regression failed: $test" }
        if ($test -eq 'pool_trace_reader_tests' -and !$AllowNativeEtlSkip -and
            ($poolTestOutput -join "`n") -notmatch 'POOL_NATIVE_ETL_INTEGRATION=PASS PRIVATE_IN_PROC_SYNTHETIC_PROVIDER') {
            throw 'The native ETL integration did not pass. A skipped integration is not a successful end-to-end check.'
        }
    }
} finally { Pop-Location }
