# Headless screenshots of the lock screen with "Already have an account?" (pending / denied, closed and open). FAKE data, no network, muted.
# Usage: qa\lock_signin_shots.ps1 [outdir]   (writes .bmp and .png)
param([string]$Out = "$env:TEMP\lock_signin_shots")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $bmp = Join-Path $Out "$name-$size.bmp"
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = $bmp;
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5" } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  if (Test-Path $bmp) { $img = [System.Drawing.Image]::FromFile($bmp); $img.Save(($bmp -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose() }
  Write-Host "shot $name $size"
}
foreach ($size in @("1280x800", "1600x1000")) {
  foreach ($m in @("pending", "pending-signin", "unavailable", "unavailable-signin")) {
    Shot "lock_$m" $size @{ HUB_SHOT_ACCESS = $m }
  }
}
