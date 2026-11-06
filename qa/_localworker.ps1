# Dot-source from qa scripts:  . "$PSScriptRoot\_localworker.ps1"
#   $w = Start-LocalWorker -Port 8851 -Tmp $tmp        # a LOCAL wrangler dev worker (loopback, isolated registry + persist dir); returns @{ Process; Url }
#   ... run the headless peers against $w.Url ...
#   Stop-LocalWorker $w                                  # kills only the process tree this script started (by PID, never by image name)
# Never production: Assert-LocalServer (qa\_guard.ps1) is applied to the URL.
. "$PSScriptRoot\_guard.ps1"

function Start-LocalWorker([int]$Port, [string]$Tmp,
                           [string]$WorkerDir = $(if ($env:BS_WORKER_DIR) { $env:BS_WORKER_DIR } else { 'C:\Users\fatty\Desktop\SmashClassicPort\BattleShip\server\cloudflare' })) {
    if (-not (Test-Path (Join-Path $WorkerDir 'node_modules\wrangler\bin\wrangler.js'))) { throw "no wrangler found under $WorkerDir (BattleShip\server\cloudflare\node_modules)" }
    $url = "ws://127.0.0.1:$Port"
    Assert-LocalServer $url
    New-Item -ItemType Directory -Force $Tmp | Out-Null
    $env:WRANGLER_REGISTRY_PATH = Join-Path $Tmp 'wrangler-registry'   # never share the registry with another agent's worker
    $log = Join-Path $Tmp 'wrangler.log'
    $node = (Get-Command node).Source
    $p = Start-Process -FilePath $node -ArgumentList @('node_modules\wrangler\bin\wrangler.js', 'dev', '--ip', '127.0.0.1', '--port', $Port, '--persist-to', (Join-Path $Tmp 'persist'), '--var', 'REG_APPROVAL_DEFAULT:0', 'ALLOW_TEST_FLAG:1') `
        -WorkingDirectory $WorkerDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $up = $false
    for ($i = 0; $i -lt 120 -and -not $up; $i++) {
        Start-Sleep -Seconds 1
        if ($p.HasExited) { throw "local worker exited early (see $log)" }
        try { $null = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/" -TimeoutSec 2; $up = $true } catch { if ($_.Exception.Response) { $up = $true } }
    }
    if (-not $up) { Stop-LocalWorker @{ Process = $p }; throw "local worker did not start in time (see $log)" }
    return @{ Process = $p; Url = $url; Log = $log }
}

function Stop-LocalWorker($w) {
    if ($w -and $w.Process -and -not $w.Process.HasExited) {
        & taskkill.exe /PID $w.Process.Id /T /F 2>&1 | Out-Null   # the node process and its workerd child, by PID
    }
}
