param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$outputDirectory = Join-Path $repository '.codex-tmp/storage-controller-client-tests'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$compiler = (Get-Command g++ -ErrorAction Stop).Source
$testSource = Join-Path $repository 'tools/storage_controller_client_tests.cpp'
$clientSource = Join-Path $repository 'Ksword5.1/Ksword5.1/ArkDriverClient/ArkStorageControllerClient.cpp'
$transportStubs = Join-Path $repository 'tools/storage_controller_transport_stubs.h'
$testExecutable = Join-Path $outputDirectory 'storage_controller_client_tests.exe'
& $compiler '-std=c++17' '-Wall' '-Wextra' '-Werror' '-Wno-unknown-pragmas' '-O2' '-static' '-DUNICODE' '-D_UNICODE' '-include' $transportStubs $testSource $clientSource '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw "Storage controller client tests failed to compile: $LASTEXITCODE" }
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw "Storage controller client tests failed: $LASTEXITCODE" }
