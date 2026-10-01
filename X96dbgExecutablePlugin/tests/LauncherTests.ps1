[CmdletBinding()]
param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$launcher = Join-Path $repositoryRoot "X96dbgExecutablePlugin\x64\$Configuration\KswordX96dbgLauncher.exe"
if (!(Test-Path -LiteralPath $launcher -PathType Leaf)) { throw 'Build the x64 launcher first.' }

# Only read-only commands and deliberately invalid launch commands are used.
# These checks never create a window, debugger session or target process.
$infoText = & $launcher --ksword-plugin info
if ($LASTEXITCODE -ne 0) { throw 'info command failed.' }
$info = $infoText | ConvertFrom-Json
if ($info.protocol -ne 'ksword-plugin/1' -or $info.plugin_id -ne 'x96dbg' -or $info.architecture -ne 'x64' -or !$info.native_without_driver) { throw 'Launcher capability contract mismatch.' }
foreach ($arguments in @(
    @('--ksword-plugin', 'attach', '--pid', '0'),
    @('--ksword-plugin', 'attach', '--pid', '-1'),
    @('--ksword-plugin', 'attach', '--pid', '4294967296'),
    @('--ksword-plugin', 'tab', '--parent-hwnd', '0', '--host-pid', '1'),
    @('--ksword-plugin', 'info', '--unknown', 'x')
)) {
    $responseText = & $launcher @arguments
    if ($LASTEXITCODE -ne 2) { throw "Invalid command was accepted: $($arguments -join ' ')" }
    $response = $responseText | ConvertFrom-Json
    if ($response.event -ne 'error' -or $response.code -ne 'invalid_arguments') { throw 'Invalid-command error contract mismatch.' }
}

$script = Join-Path $repositoryRoot 'tools\package_x96dbg_plugin.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw ($errors | Out-String) }
# Execute only the pure PE-reading function definitions, not package mutations.
$functions = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false)
foreach ($function in $functions) { . ([scriptblock]::Create($function.Extent.Text)) }
$pe = Get-Amd64Pe $launcher
if ($pe.Machine -ne 'AMD64') { throw 'Launcher is not AMD64.' }
$badPe = Join-Path $repositoryRoot '.codex-tmp\x96dbg-bad-pe.bin'
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($badPe)) -Force | Out-Null
[IO.File]::WriteAllBytes($badPe, [byte[]](0x4D, 0x5A, 0x00))
$rejected = $false
try { $null = Get-Amd64Pe $badPe } catch { $rejected = $true }
if (!$rejected) { throw 'Truncated PE was accepted.' }
Write-Output 'PASS: launcher JSONL capabilities, invalid launch arguments, AMD64 PE contract, truncated-PE rejection, package PowerShell syntax. No GUI was launched.'
exit 0
