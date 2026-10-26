# Headless key-frame screenshots of the game start animation (HUB_SHOT_BOOTANIM=<system>:<t seconds>), 1280x800, converted to PNG.
# Usage: qa\bootanim_shots.ps1 [outdir] [systems csv] [times csv]      No window is ever shown, no network, fake library.
param([string]$Out = "$env:TEMP\bootanim_shots", [string]$Systems = "n64,gba,nds,gc,ps1,wiiu", [string]$Times = "0.3,0.9,1.5,2.0,2.8")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($sys in $Systems.Split(",")) {
  foreach ($t in $Times.Split(",")) {
    $name = "boot_${sys}_$t"
    $bmp = Join-Path $Out "$name.bmp"
    Remove-Item $bmp -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $root "build\bin\Release\userdata\hub.json") -ErrorAction SilentlyContinue
    $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = "1280x800";
              HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "2"; HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_BOOTANIM = "${sys}:$t" }
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $p = Start-Process $exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill() }
    foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path $bmp) {
      $img = [System.Drawing.Image]::FromFile($bmp)
      $img.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
      $img.Dispose()
      Remove-Item $bmp
      Write-Host "OK   $name"
    } else { Write-Host "FAIL $name (no frame)" }
  }
}
