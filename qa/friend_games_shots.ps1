# Headless screenshots of the greyed-out friends' games (fake friends and fake libraries, no network, nothing visible).
# Usage: qa\friend_games_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\friend_games_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "10"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_LIB = "all"; HUB_SHOT_FAKE_FRIEND_LIBS = "40" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in @("1280x800", "1600x1000")) {
  Shot "home" $size @{}
  Shot "all" $size @{ HUB_START_VIEW = "all" }
  Shot "all_gc" $size @{ HUB_START_VIEW = "all"; HUB_START_SYS = "gc" }
  Shot "details" $size @{ HUB_START_VIEW = "all"; HUB_SHOT_GHOST_OPEN = "3" }
  Shot "all_700" $size @{ HUB_START_VIEW = "all"; HUB_SHOT_FAKE_FRIEND_LIBS = "700" }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
