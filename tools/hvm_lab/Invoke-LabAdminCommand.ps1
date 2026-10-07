#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SessionDirectory,
    [Parameter(Mandatory)][string]$ScriptPath, [int]$WaitSeconds = 20)
$ErrorActionPreference = 'Stop'
$ready = Get-Content -LiteralPath (Join-Path $SessionDirectory 'ready.json') -Raw | ConvertFrom-Json
if ($ready.status -ne 'Ready' -or -not $ready.administrator) { throw 'The administrator session is not ready.' }
$workerProcess = Get-Process -Id $ready.pid -ErrorAction Stop
if ($workerProcess.ProcessName -ne 'powershell') { throw 'The recorded worker is not PowerShell.' }
$jobId = [guid]::NewGuid().ToString('N')
$queuedScript = Join-Path $SessionDirectory ($jobId + '.ps1')
Copy-Item -LiteralPath $ScriptPath -Destination $queuedScript
$job = @{ id = $jobId; sha256 = (Get-FileHash -LiteralPath $queuedScript -Algorithm SHA256).Hash }
$temporary = Join-Path $SessionDirectory ($jobId + '.temporary')
[IO.File]::WriteAllText($temporary, ($job | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $temporary -Destination (Join-Path $SessionDirectory ($jobId + '.request.json'))
$resultPath = Join-Path $SessionDirectory ($jobId + '.result.json')
$deadline = [DateTime]::UtcNow.AddSeconds([Math]::Min([Math]::Max($WaitSeconds, 0), 55))
while (-not (Test-Path -LiteralPath $resultPath) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 200 }
if (Test-Path -LiteralPath $resultPath) { Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json }
else { [pscustomobject]@{ status = 'Pending'; id = $jobId; resultPath = $resultPath; note = 'The queued command may still be running; no driver rollback is implied.' } }
