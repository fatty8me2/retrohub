# Headless gallery screenshots of the boot-animation art (HUB_SHOT_BOOTART=<system|all|shelf>), 1280x800 PNGs. No window is shown.
# Usage: qa\bootart_gallery.ps1 [outdir] [systems csv]
param([string]$Out = "$env:TEMP\bootart_gallery", [string]$Systems = "all,n64,nes,snes,gb,gbc,gba,ds,3ds,gc,ps1,ps2,wiiu")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($sys in $Systems.Split(",")) {
  $bmp = Join-Path $Out "art_$sys.bmp"
  Remove-Item $bmp -ErrorAction SilentlyContinue
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = "1280x800";
            HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "2"; HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_BOOTART = $sys }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  if (Test-Path $bmp) {
    $img = [System.Drawing.Image]::FromFile($bmp)
    $img.Save((Join-Path $Out "art_$sys.png"), [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose(); Remove-Item $bmp
    Write-Host "OK   art_$sys"
  } else { Write-Host "FAIL art_$sys" }
}
