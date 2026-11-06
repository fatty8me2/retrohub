# Headless screenshots of invite codes: the waiting screen code field, the Admin invite card and pending queue, the owner bell (FAKE data).
# No network, no visible window, muted. Usage: qa\invite_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\invite_shots")
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
  Shot "access_pending_code" $size @{ HUB_SHOT_ACCESS = "pending-code" }
  Shot "access_pending_bad" $size @{ HUB_SHOT_ACCESS = "pending-bad" }
  Shot "access_pending" $size @{ HUB_SHOT_ACCESS = "pending" }
  Shot "admin_pending_queue" $size @{ HUB_SHOT_ADMIN = "pending" }
  Shot "admin_invite" $size @{ HUB_SHOT_ADMIN = "invite" }
  Shot "bell_owner" $size @{ HUB_SHOT_ADMIN = "bell"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_BELL = "1" }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
