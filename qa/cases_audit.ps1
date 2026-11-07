# Cover-art coverage audit of the Cases view (HUB_CASES_AUDIT) on a COPY of a real library: one TSV row per Cases entry (owned games, friend ghosts, pack ghosts).
# Usage: qa\cases_audit.ps1 -Exe <dir with RetroHub.exe + data\> -UserData <userdata COPY> -Out <out.tsv> [-Friends <friend_libs.json>] [-Pack <pack index tsv>] [-Secs 240]
#   -Friends / -Pack default to <UserData>\friend_libs.json and none. Make the pack file with:  hub_metadata_prefetch <ud> --ghosts-only --pack-live <pack.tsv>
# Headless, muted, never a visible window; the cover fetches are the app's own (polite, rate limited). The process is killed by PID if it overruns.
# NEVER point -UserData at the real data folder: the art cache and names.json / misses.json are written.
param([Parameter(Mandatory = $true)][string]$Exe, [Parameter(Mandatory = $true)][string]$UserData, [Parameter(Mandatory = $true)][string]$Out,
      [string]$Friends = "", [string]$Pack = "", [int]$Secs = 240)
$ErrorActionPreference = "Stop"
if ($UserData -match 'smashClassic\\userdata') { throw "refusing: not an isolated copy" }
$Exe = (Resolve-Path $Exe).Path; $UserData = (Resolve-Path $UserData).Path
# RetroHub keeps its data in <exe dir>\userdata: run a private copy of the exe folder next to a link to the userdata copy
$run = Join-Path (Split-Path $UserData -Parent) "audit_run"
if (Test-Path $run) { Remove-Item $run -Recurse -Force }
New-Item -ItemType Directory -Force $run | Out-Null
Copy-Item (Join-Path $Exe "RetroHub.exe"), (Join-Path $Exe "SDL2.dll") $run
if (Test-Path (Join-Path $Exe "data")) { Copy-Item (Join-Path $Exe "data") $run -Recurse }
Copy-Item $UserData (Join-Path $run "userdata") -Recurse
if (-not $Friends) { $Friends = Join-Path $UserData "friend_libs.json" }
$bmp = Join-Path $run "audit.bmp"
$set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp; HUB_START_PAGE = "0"; HUB_START_VIEW = "all"; HUB_SHOT_SIZE = "1600x1000"
          HUB_SHOT_SECS = "$($Secs + 60)"; HUB_CASES_AUDIT = $Out; HUB_CASES_AUDIT_SECS = "$Secs" }
if (Test-Path $Friends) { $set["HUB_CASES_AUDIT_FRIENDS"] = $Friends }
if ($Pack) { $set["HUB_CASES_AUDIT_PACK"] = (Resolve-Path $Pack).Path }
if (Test-Path $Out) { Remove-Item $Out }
try {
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process (Join-Path $run "RetroHub.exe") -WorkingDirectory $run -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $run "stderr.txt")
  $p.PriorityClass = 'BelowNormal'
  if (-not $p.WaitForExit(($Secs + 120) * 1000)) { Stop-Process -Id $p.Id -Force; Write-Host "killed (overran)" }
} finally { foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
if (Test-Path $Out) { Write-Host (Get-Content (Join-Path $run "stderr.txt") | Select-String "cases audit"); Write-Host "wrote $Out" } else { Write-Host "NO AUDIT FILE"; Get-Content (Join-Path $run "stderr.txt") -ErrorAction SilentlyContinue | Select-Object -Last 5 }
