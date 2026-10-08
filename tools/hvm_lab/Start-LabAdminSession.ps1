#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SessionDirectory, [switch]$Worker)
$ErrorActionPreference = 'Stop'
if (-not $Worker) {
    $null = New-Item -ItemType Directory -Path $SessionDirectory -Force
    $SessionDirectory = (Resolve-Path -LiteralPath $SessionDirectory).Path
    $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @($userSid.Value, 'S-1-5-18', 'S-1-5-32-544')) {
        $rule = [Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($sid), 'FullControl',
            'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $SessionDirectory -AclObject $acl
    $workerScript = Join-Path $SessionDirectory 'AdminWorker.ps1'
    Copy-Item -LiteralPath $PSCommandPath -Destination $workerScript
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -SessionDirectory "{1}" -Worker' -f $workerScript, $SessionDirectory
    $process = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -PassThru
    [pscustomobject]@{ status = 'Started'; pid = $process.Id; sessionDirectory = $SessionDirectory }
    return
}
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'The worker requires administrator privileges.' }
$utf8 = [Text.UTF8Encoding]::new($false)
function Save-Json($Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), $utf8) }
$readyPath = Join-Path $SessionDirectory 'ready.json'
Save-Json $readyPath @{ pid = $PID; administrator = $true; startedUtc = [DateTime]::UtcNow.ToString('o'); status = 'Ready' }
try {
    while (-not (Test-Path -LiteralPath (Join-Path $SessionDirectory 'stop.flag'))) {
        $request = Get-ChildItem -LiteralPath $SessionDirectory -Filter '*.request.json' | Sort-Object Name | Select-Object -First 1
        if (-not $request) { Start-Sleep -Milliseconds 300; continue }
        $job = Get-Content -LiteralPath $request.FullName -Raw | ConvertFrom-Json
        $jobId = [guid]::Parse($job.id).ToString('N')
        $scriptPath = Join-Path $SessionDirectory ($jobId + '.ps1')
        $resultPath = Join-Path $SessionDirectory ($jobId + '.result.json')
        Save-Json (Join-Path $SessionDirectory 'active.json') @{ id = $jobId; pid = $PID; startedUtc = [DateTime]::UtcNow.ToString('o') }
        $result = @{ id = $jobId; success = $false; startedUtc = [DateTime]::UtcNow.ToString('o'); output = ''; error = '' }
        try {
            $actualHash = (Get-FileHash -LiteralPath $scriptPath -Algorithm SHA256).Hash
            if ($actualHash -ne $job.sha256) { throw 'Queued script hash mismatch.' }
            $result.output = (& $scriptPath *>&1 | Out-String -Width 4096)
            $result.success = $true
        } catch { $result.error = ($_ | Out-String -Width 4096) }
        $result.finishedUtc = [DateTime]::UtcNow.ToString('o')
        Save-Json $resultPath $result
        Rename-Item -LiteralPath $request.FullName -NewName ($jobId + '.handled.json')
    }
} finally { Save-Json $readyPath @{ pid = $PID; administrator = $true; status = 'Stopped'; stoppedUtc = [DateTime]::UtcNow.ToString('o') } }
