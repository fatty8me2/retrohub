# End-to-end run of invite codes against a LOCAL worker (never production): the REAL FriendsClient + lock verdict (src\net\qa_invite_e2e.cpp).
# Starts wrangler dev from the BattleShip worktree's server\cloudflare folder (isolated persist dir under C:\rhq\inv), runs the scenarios, stops the worker by PID.
# Usage: qa\invite_e2e.ps1 [-Server <path to server\cloudflare>] [-Port 10320]
param([string]$Server = "C:\Users\fatty\Desktop\SmashClassicPort\wt\invites-bs\server\cloudflare", [int]$Port = 10320)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$stop = "C:\rhq\inv_stop"; $log = "C:\rhq\inv_srv.log"
New-Item -ItemType Directory -Force "C:\rhq\inv" | Out-Null
Remove-Item $stop -ErrorAction SilentlyContinue
$env:RHQ_ROOT = "C:\rhq\inv"
$p = Start-Process node -ArgumentList @((Join-Path $PSScriptRoot "invite_e2e_server.mjs"), $Server, $Port, $stop) -PassThru -WindowStyle Hidden -RedirectStandardOutput $log
try {
  for ($i = 0; $i -lt 120 -and -not ((Test-Path $log) -and (Select-String -Path $log -Pattern "READY" -Quiet)); $i++) { Start-Sleep -Milliseconds 500 }
  Get-Content $log
  $env:SSB64_MUTE = "1"
  & (Join-Path $root "build\bin\Release\qa_invite_e2e.exe") "ws://127.0.0.1:$Port" "C:\rhq\inv_work"
  $rc = $LASTEXITCODE
} finally {
  New-Item -ItemType File $stop -Force | Out-Null   # the server script stops its own wrangler (by PID) when this file appears
  $p.WaitForExit(15000) | Out-Null
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  Remove-Item $stop -ErrorAction SilentlyContinue
}
exit $rc
