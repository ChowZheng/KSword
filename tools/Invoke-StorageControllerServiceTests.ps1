param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$outputDirectory = Join-Path $repository '.codex-tmp/storage-controller-service-tests'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$testSource = Join-Path $repository 'tools/storage_controller_service_tests.cpp'
$serviceSource = Join-Path $repository 'Ksword5.1/Ksword5.1/ksword/service/service.cpp'
$stringSource = Join-Path $repository 'Ksword5.1/Ksword5.1/ksword/string/string.cpp'
$transportStubs = Join-Path $repository 'tools/storage_controller_service_stubs.h'
$testExecutable = Join-Path $outputDirectory 'storage_controller_service_tests.exe'
& $compiler '-std=c++17' '-Wall' '-Wextra' '-Werror' '-Wno-unknown-pragmas' '-O2' '-static' '-DUNICODE' '-D_UNICODE' '-include' $transportStubs $testSource $serviceSource $stringSource '-ladvapi32' '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw "Storage controller service tests failed to compile: $LASTEXITCODE" }
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw "Storage controller service tests failed: $LASTEXITCODE" }
