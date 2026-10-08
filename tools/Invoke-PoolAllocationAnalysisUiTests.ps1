[CmdletBinding()]
param(
    [string]$QtRoot = '.codex-tmp/qt-fixture/ucrt64',
    [string]$OutputDirectory = '.codex-tmp/pool-analysis-ui-tests'
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$previousPath = $env:PATH
$previousPlatform = $env:QT_QPA_PLATFORM
Push-Location $repository
try {
    $qt = (Resolve-Path -LiteralPath $QtRoot).Path
    $include = Join-Path $qt 'include/qt6'
    if (!(Test-Path -LiteralPath $include)) { $include = Join-Path $qt 'include' }
    $output = [IO.Path]::GetFullPath((Join-Path $repository $OutputDirectory))
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $sourceRoot = 'Ksword5.1/Ksword5.1'
    $arguments = @('-std=c++17', '-O1', '-g0', '-Wall', '-Wextra', '-Werror', '-DUNICODE', '-D_UNICODE', '-DNOMINMAX',
        '-isystem', $include, '-isystem', (Join-Path $include 'QtCore'),
        '-isystem', (Join-Path $include 'QtGui'), '-isystem', (Join-Path $include 'QtWidgets'), '-isystem', (Join-Path $include 'QtTest'),
        'tools/pool_allocation_analysis_ui_tests.cpp',
        "$sourceRoot/MemoryDock/PoolAllocationAnalysisWidget.cpp",
        "$sourceRoot/UI/FlowLayout.cpp",
        "$sourceRoot/Internationalization/LanguageManager.cpp",
        'shared/evidence/PoolAllocationAnalysis.cpp',
        "-L$qt/lib", '-lQt6Widgets', '-lQt6Gui', '-lQt6Core', '-lQt6Test',
        '-o', (Join-Path $output 'pool-analysis-ui-tests.exe'))
    & g++ @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Pool allocation Qt fixture compilation failed.' }
    $env:PATH = (Join-Path $qt 'bin') + ';' + $previousPath
    $env:QT_QPA_PLATFORM = 'offscreen'
    & (Join-Path $output 'pool-analysis-ui-tests.exe') $output
    if ($LASTEXITCODE -ne 0) { throw 'Pool allocation Qt fixture failed.' }
} finally {
    $env:PATH = $previousPath
    $env:QT_QPA_PLATFORM = $previousPlatform
    Pop-Location
}
