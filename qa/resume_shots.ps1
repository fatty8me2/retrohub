# Headless screenshots for Wave 3 R1: the Saves-tab "Which save to start from" row and the Play choice (made-up values, no game starts).
# Usage: qa\resume_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\resume_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5" } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in @("1280x800", "1600x1000")) {
  Shot "resume_saves_row" $size @{ HUB_SHOT_RESUME = "1"; HUB_SHOT_DETAILS = "fake:0"; HUB_SHOT_TAB = "1" }
  Shot "resume_play_choice" $size @{ HUB_SHOT_RESUME = "1"; HUB_SHOT_EMU = "resume" }
}
