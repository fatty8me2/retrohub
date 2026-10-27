# Headless screenshots for Wave 3 L6 (Attract mode), L7 (Surprise me) and L8 (RetroHub Wrapped), converted to PNG.
# Usage: qa\homefun_shots.ps1 [outdir] [size WxH]      No window is ever shown (SDL dummy driver), muted, no network, fake library.
# Also writes <outdir>\trace.txt (HUB_SHOT_TRACE lines: attract start/stop, surprise pick/open, wrapped-save path).
param([string]$Out = "$env:TEMP\homefun_shots", [string]$Size = "1280x800")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$trace = Join-Path $Out "trace.txt"
Remove-Item $trace -ErrorAction SilentlyContinue

function Shot([string]$name, [hashtable]$extra) {
  $bmp = Join-Path $Out "$name.bmp"
  Remove-Item $bmp -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $root "build\bin\Release\userdata\hub.json") -ErrorAction SilentlyContinue
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = $Size;
            HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "3"; HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_STATES = "1"; HUB_SHOT_TRACE = $trace }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  $psi = New-Object System.Diagnostics.ProcessStartInfo $exe
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.WorkingDirectory = Split-Path $exe -Parent
  foreach ($k in $set.Keys) { $psi.EnvironmentVariables[$k] = [string]$set[$k] }
  Add-Content $trace "== $name"
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

# L7: the dice button on Home, then mid-spin and the details modal it opens
Shot "home_dice" @{}
Shot "surprise_spin" @{ HUB_SHOT_SURPRISE = "3"; HUB_SHOT_SECS = "2.9" }
Shot "surprise_landed" @{ HUB_SHOT_SURPRISE = "3"; HUB_SHOT_SECS = "4.7" }
Shot "surprise_details" @{ HUB_SHOT_SURPRISE = "3"; HUB_SHOT_SECS = "7" }
# L6: attract mode (idle time shortened to 1 s), a crossfade, and woken by a key press
Shot "attract_a" @{ HUB_ATTRACT_SECS = "1"; HUB_SHOT_SECS = "4" }
Shot "attract_fade" @{ HUB_ATTRACT_SECS = "1"; HUB_SHOT_SECS = "6.9" }
Shot "attract_woken" @{ HUB_ATTRACT_SECS = "1"; HUB_SHOT_SECS = "5"; HUB_SHOT_KEYS = "right" }
Shot "attract_off_home" @{ HUB_ATTRACT_SECS = "1"; HUB_SHOT_SECS = "4"; HUB_SHOT_PAGE = "settings" }
# L8: Wrapped, every card, month and year, then the exported picture
foreach ($c in 0..4) { Shot "wrapped_month_$c" @{ HUB_SHOT_WRAPPED = "month"; HUB_SHOT_WRAPPED_FAKE = "1"; HUB_SHOT_WRAPPED_CARD = "$c"; HUB_SHOT_SECS = "3" } }
Shot "wrapped_year_0" @{ HUB_SHOT_WRAPPED = "year"; HUB_SHOT_WRAPPED_FAKE = "1"; HUB_SHOT_WRAPPED_CARD = "1"; HUB_SHOT_SECS = "3" }
Shot "wrapped_empty" @{ HUB_SHOT_WRAPPED = "month"; HUB_SHOT_SECS = "3" }
Shot "wrapped_save" @{ HUB_SHOT_WRAPPED = "month"; HUB_SHOT_WRAPPED_FAKE = "1"; HUB_SHOT_WRAPPED_SAVE = "1"; HUB_SHOT_SECS = "4" }
$saved = Get-Content $trace | Where-Object { $_ -like "wrapped-save *" } | Select-Object -Last 1
if ($saved) {
  $path = $saved.Substring("wrapped-save ".Length)
  if (Test-Path $path) { Copy-Item $path (Join-Path $Out "wrapped_sheet.png") -Force; Write-Host "ok  wrapped_sheet ($path)" } else { Write-Host "MISSING wrapped_sheet: $saved" }
}
Get-Content $trace
