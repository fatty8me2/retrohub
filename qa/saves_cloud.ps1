# Cloud saves end-to-end (headless): starts a local `wrangler dev` (emulated R2) from the BattleShip Worker worktree,
# registers two accounts, runs hubsaves_cloud_test (two simulated PCs + isolation + dead-network cases), stops it.
#   powershell -ExecutionPolicy Bypass -File qa\saves_cloud.ps1 [-WorkerDir <...\server\cloudflare>]
param([string]$WorkerDir = "")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $WorkerDir) { $WorkerDir = Join-Path $Root "..\cloud-saves-server\server\cloudflare" }
$WorkerDir = (Resolve-Path $WorkerDir).Path
$Test = Join-Path $Root "build\bin\Release\hubsaves_cloud_test.exe"
$Acc = Join-Path $env:TEMP "rh_cloud_accounts.json"
Remove-Item $Acc -ErrorAction SilentlyContinue
$p = Start-Process node -ArgumentList @("test_saves.mjs", "--serve", "`"$Acc`"") -WorkingDirectory $WorkerDir -PassThru -WindowStyle Hidden
try {
    for ($i = 0; $i -lt 120 -and -not (Test-Path $Acc); $i++) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $Acc)) { throw "dev server did not come up" }
    $env:RETROHUB_TEST_SAVES_ACCOUNTS = $Acc
    & $Test
    exit $LASTEXITCODE
} finally {
    if ($p -and -not $p.HasExited) { & taskkill /PID $p.Id /T /F | Out-Null }
}
