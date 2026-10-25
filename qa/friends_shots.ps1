# Headless screenshots for Wave 2 lane 3 (friends playing shelf, ask-to-join toast, room ping pills). No network, fake friends/room.
# Usage: qa\friends_shots.ps1 [outdir]    Opens nothing visible; outputs BMP + PNG per scene and size.
param([string]$Out = "$env:TEMP\friends_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5" } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in @("1280x800", "1600x1000")) {
  Shot "home_friends" $size @{ HUB_SHOT_FRIENDS = "1"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_LIB = "8"; HUB_SHOT_FAKE_ASK = "1"; HUB_SHOT_HOVER = "kaiuid" }
  Shot "home_nopanel" $size @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_LIB = "8" }
  Shot "room_ping" $size @{ HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_N = "4"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_READY = "1,2" }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
