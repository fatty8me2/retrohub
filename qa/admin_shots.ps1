# Headless screenshots of the owner dashboard (Admin page) with FAKE data, and of the Play Online notice for a waiting / blocked account.
# No network, no visible window, muted. Usage: qa\admin_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\admin_shots")
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
  Shot "admin_accounts" $size @{ HUB_SHOT_ADMIN = "1" }
  Shot "admin_pending" $size @{ HUB_SHOT_ADMIN = "pending" }
  Shot "admin_detail" $size @{ HUB_SHOT_ADMIN = "detail" }
  Shot "admin_confirm" $size @{ HUB_SHOT_ADMIN = "confirm" }
  Shot "admin_settings" $size @{ HUB_SHOT_ADMIN = "settings" }
  Shot "access_pending" $size @{ HUB_SHOT_ACCESS = "pending" }
  Shot "access_banned" $size @{ HUB_SHOT_ACCESS = "banned" }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
