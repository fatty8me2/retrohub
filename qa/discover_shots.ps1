# Headless screenshots of the Discover page with a fake library (every 5th recommendation owned) and a fake shared pack
# (the 2nd and 4th of each five installable). No network, muted, no window. Usage: qa\discover_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\discover_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $console, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL="1"; RETROHUB_QA="1"; SSB64_MUTE="1"; RETRORUN_MUTE="1"; N64RUN_MUTE="1"
            HUB_SCREENSHOT=(Join-Path $Out "$name.bmp"); HUB_SHOT_SIZE=$size; HUB_SHOT_SECS="6"
            HUB_SHOT_PAGE="discover:$console"; HUB_SHOT_FAKE_LIB="empty"; HUB_SHOT_DISCOVER_FAKE="1" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name"
}
Shot "discover_n64_1280" "n64" "1280x800" @{}
Shot "discover_snes_1280" "snes" "1280x800" @{}
Shot "discover_ps2_1600" "ps2" "1600x1000" @{}
Shot "discover_gba_friends_1280" "gba" "1280x800" @{ HUB_SHOT_DISCOVER_FRIENDS="1" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
