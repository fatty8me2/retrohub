# Plan 4 H1/H3/M1, end to end with the REAL RetroHub.exe in `--apply-update` mode and a REAL long-running process under the install folder:
# an update never closes a game. Scratch folders only; nothing visible (RetroHub.exe in apply mode has no window), nothing relaunched
# (--no-relaunch), no network, never the user's install or settings.
#
# The "game" is a copy of ping.exe renamed n64run.exe and kept under the scratch install folder (so its image path is under the install, exactly
# like a real runner or emulator). Only the pids this script started are ever ended.
#
#   1. A game is still running when the applier starts, and stays running for longer than --wait-secs:
#        the applier gives up (exit 3), changes NOTHING, leaves a "postponed" status note and keeps the staged files; the game is still alive.
#   2. The game closes by itself while the applier waits:
#        the applier carries on (exit 0), every file is replaced, the status note is gone, the staged files are removed; the game was never touched.
#   3. A file is locked (antivirus-style): the applier rolls back, leaves a "failed" note with a plain reason and keeps the staged files.
#
# usage: qa\update_during_game.ps1 [-Build <bin\Release>] [-Work <scratch dir>]
param(
  [string]$Build = (Join-Path (Split-Path $PSScriptRoot) "build\bin\Release"),
  [string]$Work = (Join-Path $env:TEMP ("rh_update_during_game_" + [guid]::NewGuid().ToString("N").Substring(0, 8)))
)
$ErrorActionPreference = "Stop"
$hub = Join-Path $Build "RetroHub.exe"
if (-not (Test-Path $hub)) { throw "RetroHub.exe not found in $Build (build the RetroHub target first)" }
$ping = Join-Path $env:WINDIR "System32\PING.EXE"
$script:fail = 0
$script:started = New-Object System.Collections.ArrayList
function Pass($m) { Write-Host "PASS: $m" }
function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }
function Put($path, $text) { New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null; [IO.File]::WriteAllText($path, $text) }
function Get-Text($path) { if (Test-Path $path) { [IO.File]::ReadAllText($path) } else { $null } }

# a scratch install (old files + user data that must survive) and a staged update (new files)
function New-Scene([string]$name) {
  $root = Join-Path $Work $name
  if (Test-Path $root) { Remove-Item $root -Recurse -Force }
  $inst = Join-Path $root "Install"; $stg = Join-Path $root "staged"
  Put "$stg\RetroHub.exe" "new-hub"; Put "$stg\BattleShip.exe" "new-bs"; Put "$stg\a.txt" "a-new"; Put "$stg\b.dat" "b-new"; Put "$stg\sub\deep\c.txt" "c-new"
  Put "$inst\RetroHub.exe" "old-hub"; Put "$inst\BattleShip.exe" "old-bs"; Put "$inst\a.txt" "a-old"; Put "$inst\b.dat" "b-old"
  Put "$inst\userdata\hub.json" "user-settings"; Put "$inst\userdata\saves\mario.sav" "user-save"; Put "$inst\extra.txt" "keep me"
  foreach ($f in Get-ChildItem $inst -Recurse -File) { $f.LastWriteTime = (Get-Date).AddDays(-2) }   # installed files are older than the staged ones
  [pscustomobject]@{ Root = $root; Install = $inst; Staged = $stg; Tmp = (Join-Path $root "tmp") }
}
function Start-Game($scene, [int]$seconds) {
  $game = Join-Path $scene.Install "n64run.exe"
  Copy-Item $ping $game
  $p = Start-Process $game -ArgumentList @("-n", "$seconds", "127.0.0.1") -PassThru -WindowStyle Hidden
  [void]$script:started.Add($p.Id)
  Start-Sleep -Milliseconds 700
  $p
}
function Run-Applier($scene, [string]$extra) {
  $argList = @("--apply-update", $scene.Staged, $scene.Install, "0", "--no-relaunch", "--version", "v9.9.9", "--from", "v1.0.0", "--tmp-root", $scene.Tmp) + $extra.Split(" ", [StringSplitOptions]::RemoveEmptyEntries)
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process $hub -ArgumentList $argList -PassThru -Wait -WindowStyle Hidden
  [pscustomobject]@{ Exit = $p.ExitCode; Secs = $sw.Elapsed.TotalSeconds }
}
function Is-Old($scene) { (Get-Text "$($scene.Install)\RetroHub.exe") -eq "old-hub" -and (Get-Text "$($scene.Install)\a.txt") -eq "a-old" -and (Get-Text "$($scene.Install)\b.dat") -eq "b-old" -and -not (Test-Path "$($scene.Install)\sub") }
function Is-New($scene) { (Get-Text "$($scene.Install)\RetroHub.exe") -eq "new-hub" -and (Get-Text "$($scene.Install)\a.txt") -eq "a-new" -and (Get-Text "$($scene.Install)\b.dat") -eq "b-new" -and (Get-Text "$($scene.Install)\sub\deep\c.txt") -eq "c-new" }
function User-Data-Ok($scene) { (Get-Text "$($scene.Install)\userdata\hub.json") -eq "user-settings" -and (Get-Text "$($scene.Install)\userdata\saves\mario.sav") -eq "user-save" -and (Get-Text "$($scene.Install)\extra.txt") -eq "keep me" }

try {
  New-Item -ItemType Directory -Force $Work | Out-Null

  # ---- 1. the game outlives the applier's patience ----
  $s = New-Scene "game_stays"
  $game = Start-Game $s 40
  $r = Run-Applier $s "--wait-secs 4"
  Write-Host ("applier exit {0} after {1:N1}s" -f $r.Exit, $r.Secs)
  if ($r.Exit -eq 3) { Pass "applier gave up with exit 3 (postponed) instead of closing the game" } else { Fail "applier exit was $($r.Exit), expected 3" }
  if ($r.Secs -ge 3.5) { Pass "applier really waited for the game ($([math]::Round($r.Secs,1)) s)" } else { Fail "applier did not wait" }
  if (-not $game.HasExited) { Pass "the game process is STILL RUNNING (never terminated)" } else { Fail "the game was closed by the applier" }
  if (Is-Old $s) { Pass "no file of the install was changed" } else { Fail "install was modified" }
  if (User-Data-Ok $s) { Pass "settings, saves and user files untouched" } else { Fail "user data changed" }
  $note = Get-Text "$($s.Install)\userdata\update_status.json"
  if ($note -and $note -match '"result":"postponed"' -and $note -match 'n64run\.exe') { Pass "status note written for the next start's bell: postponed, names the game" } else { Fail "no postponed status note ($note)" }
  if (Test-Path "$($s.Staged)\RetroHub.exe") { Pass "staged files kept: a retry needs no download" } else { Fail "staged files were removed" }
  Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue   # the pid this script started

  # ---- 2. the game closes while the applier waits ----
  $s = New-Scene "game_ends"
  Put "$($s.Install)\userdata\update_status.json" '{"v":1,"result":"failed","version":"v0.0.1"}'   # an older failure note
  $game = Start-Game $s 5
  $r = Run-Applier $s "--wait-secs 60"
  Write-Host ("applier exit {0} after {1:N1}s" -f $r.Exit, $r.Secs)
  if ($r.Exit -eq 0) { Pass "applier finished (exit 0) once the game had closed on its own" } else { Fail "applier exit was $($r.Exit), expected 0" }
  if ($r.Secs -ge 2.5) { Pass "applier waited for the game ($([math]::Round($r.Secs,1)) s)" } else { Fail "applier did not wait for the game" }
  if (Is-New $s) { Pass "every file replaced" } else { Fail "files not replaced" }
  if (User-Data-Ok $s) { Pass "settings, saves and user files untouched" } else { Fail "user data changed" }
  if (-not (Test-Path "$($s.Install)\userdata\update_status.json")) { Pass "the old failure note is gone after a success" } else { Fail "stale status note left" }
  if (-not (Test-Path $s.Staged)) { Pass "staged files removed after a successful install" } else { Fail "staged files left behind" }
  if (-not (Get-ChildItem $s.Tmp -Directory -Filter "RetroHub-update-*" -ErrorAction SilentlyContinue)) { Pass "no backup directory left behind" } else { Fail "backup directory left behind" }

  # ---- 3. a locked file: rolled back, explained, staged files kept ----
  $s = New-Scene "locked"
  $fs = [IO.File]::Open("$($s.Install)\b.dat", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)   # held open like an antivirus scan would
  try { $r = Run-Applier $s "--retries 1 --retry-ms 0" } finally { $fs.Close() }
  if ($r.Exit -eq 1) { Pass "applier failed cleanly (exit 1)" } else { Fail "applier exit was $($r.Exit), expected 1" }
  if (Is-Old $s) { Pass "everything rolled back to the old version" } else { Fail "install not restored" }
  $note = Get-Text "$($s.Install)\userdata\update_status.json"
  if ($note -and $note -match '"result":"failed"' -and $note -match 'in use by another program') { Pass "status note: failed, plain reason (file in use)" } else { Fail "no failed status note ($note)" }
  if (Test-Path "$($s.Staged)\RetroHub.exe") { Pass "staged files kept for a retry" } else { Fail "staged files removed" }
}
finally {
  foreach ($id in $script:started) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }   # only pids this script started
  Start-Sleep -Milliseconds 500
  Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:fail) { Write-Host "update_during_game: $($script:fail) FAILED"; exit 1 } else { Write-Host "update_during_game: all passed" }
