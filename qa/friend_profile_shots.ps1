# Headless screenshots of the friend menu, the profile view and the friends panel (fake friends, no network, muted, hidden window).
# Usage: qa\friend_profile_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\friend_profile_shots", [string[]]$Sizes = @("1280x800", "1600x1000", "1100x700"))
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5"; HUB_SHOT_FRIENDS = "1"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_PROFILES = "1"; HUB_SHOT_FAKE_LIB = "8"; HUB_SHOT_FAKE_FRIEND_LIBS = "1"; HUB_SHOT_FAKE_SHARED = "1" } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in $Sizes) {
  Shot "menu_online" $size @{ HUB_SHOT_CTX = "miauid" }
  Shot "menu_more" $size @{ HUB_SHOT_CTX = "miauid"; HUB_SHOT_CTX_MORE = "1" }
  Shot "menu_offline" $size @{ HUB_SHOT_CTX = "priuid" }
  Shot "menu_room" $size @{ HUB_SHOT_CTX = "lunuid"; HUB_SHOT_FAKE_ROOM = "1" }
  Shot "view_rich" $size @{ HUB_SHOT_VIEW = "miauid" }
  Shot "view_rich_bottom" $size @{ HUB_SHOT_VIEW = "miauid"; HUB_SHOT_VIEW_BOTTOM = "1" }
  Shot "view_offline" $size @{ HUB_SHOT_VIEW = "priuid" }
  Shot "view_me" $size @{ HUB_SHOT_VIEW = "me" }
  Shot "view_long" $size @{ HUB_SHOT_VIEW = "miauid"; HUB_SHOT_LONGNAMES = "1" }
  Shot "panel_long" $size @{ HUB_SHOT_LONGNAMES = "1"; HUB_SHOT_HOVER = "miauid" }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
