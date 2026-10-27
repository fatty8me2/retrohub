# Headless screenshots of the rivalry UI: friend actions menu with head-to-head + last-5, and the Rivals card on Your stats.
# Fake friends and fake matches (group_ui.cpp rivals_fake_seed), no network, nothing visible.
# Usage: qa\rivals_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\rivals_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "10"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_LIB = "all" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  if ($set.ContainsKey("NOLIB")) { $set.Remove("NOLIB"); $set.Remove("HUB_SHOT_FAKE_LIB") }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in @("1280x800", "1600x1000")) {
  Shot "menu_mia" $size @{ HUB_SHOT_FRIENDS = "1"; HUB_SHOT_CTX = "miauid" }      # Smash rivalry: record, mains, last 5
  Shot "menu_jake" $size @{ HUB_SHOT_FRIENDS = "1"; HUB_SHOT_CTX = "jakuid" }     # no Smash games: "sessions together"
  Shot "home_stats" $size @{ NOLIB = "1"; HUB_SHOT_SIZE = ($size -replace "x.*", "x1750") }   # tall window so Your stats (with Rivals) is on screen
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
