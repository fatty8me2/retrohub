# Headless screenshots of the merged Updates page, the 4-tab Settings, Saves and Controllers pages, converted to PNG.
# Usage: qa\ui_a_shots.ps1 [outdir] [size WxH]      No window is ever shown (SDL dummy driver), muted, no network, fake library.
param([string]$Out = "$PSScriptRoot\shots\ui-a", [string]$Size = "1600x1000")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null

function Shot([string]$name, [hashtable]$extra) {
  $bmp = Join-Path $Out "$name.bmp"
  Remove-Item $bmp -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $root "build\bin\Release\userdata\hub.json") -ErrorAction SilentlyContinue
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = $Size;
            HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "3"; HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_STATES = "1" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  $psi = New-Object System.Diagnostics.ProcessStartInfo $exe
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.WorkingDirectory = Split-Path $exe -Parent
  foreach ($k in $set.Keys) { $psi.EnvironmentVariables[$k] = [string]$set[$k] }
  $p = [System.Diagnostics.Process]::Start($psi)
  if (-not $p.WaitForExit(90000)) { $p.Kill() }   # our own PID only
  if (Test-Path $bmp) {
    $img = [System.Drawing.Image]::FromFile($bmp)
    $img.Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $bmp
    Write-Host "ok  $name"
  } else { Write-Host "MISSING $name" }
}

Shot "settings_general" @{ HUB_SHOT_PAGE = "settings:0"; HUB_SHOT_PACKLINKS = "1AbCdEfGhIjKlMnOpQrStUvWxYz0123456" }
Shot "settings_online" @{ HUB_SHOT_PAGE = "settings:1" }
Shot "settings_video" @{ HUB_SHOT_PAGE = "settings:2" }
Shot "settings_account" @{ HUB_SHOT_PAGE = "settings:3" }
Shot "saves" @{ HUB_SHOT_PAGE = "saves" }
Shot "controllers" @{ HUB_SHOT_FAKE_PADS = "3" }
Shot "updates" @{ HUB_START_PAGE = "5" }
