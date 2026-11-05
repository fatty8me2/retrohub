# Headless screenshots for Wave 3 social lane (Blocked people list, dead-end status next steps). No network, fake friends.
param([string]$Out = "$env:TEMP\social_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5"; HUB_SHOT_FAKE_FRIENDS = "1" } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in @("1280x800", "1600x1000")) {
  Shot "blocked_list" $size @{ HUB_SHOT_PAGE = "settings:1"; HUB_SHOT_BLOCKED = "1"; HUB_SHOT_WHEEL = "700,500,8" }
  Shot "status_replaced" $size @{ HUB_SHOT_FRIENDS = "1"; HUB_SHOT_FRSTATUS = "replaced" }
  Shot "status_badsecret" $size @{ HUB_SHOT_FRIENDS = "1"; HUB_SHOT_FRSTATUS = "bad-secret" }
  Shot "status_reconnect" $size @{ HUB_SHOT_FRIENDS = "1"; HUB_SHOT_FRSTATUS = "reconnect" }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
