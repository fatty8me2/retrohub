# Headless screenshots for the Home/ghost-popup tweaks (ui-b): Home with only the bundled game vs a full library, ghost details installable or not.
# Usage: qa\ui_b_shots.ps1 [outdir]   Never shows a window, muted, no network.
param([string]$Out = "$PSScriptRoot\shots\ui-b", [string]$Size = "1600x1000", [string]$Tag = "")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot([string]$name, [hashtable]$extra) {
  $bmp = Join-Path $Out "$name$Tag.bmp"
  Remove-Item $bmp -ErrorAction SilentlyContinue
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = $Size;
            HUB_SHOT_SECS = "6"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_FRIEND_LIBS = "40" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  $psi = New-Object System.Diagnostics.ProcessStartInfo $exe
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WorkingDirectory = Split-Path $exe -Parent
  foreach ($k in $set.Keys) { $psi.EnvironmentVariables[$k] = [string]$set[$k] }
  $p = [System.Diagnostics.Process]::Start($psi)
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  if (Test-Path $bmp) { $i = [System.Drawing.Image]::FromFile($bmp); $i.Save(($bmp -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $bmp }
  Write-Host "shot $name"
}
Shot "home_empty" @{ HUB_SHOT_FAKE_LIB = "native" }
Shot "home_full" @{ HUB_SHOT_FAKE_LIB = "14" }
Shot "ghost_notinstallable" @{ HUB_SHOT_FAKE_LIB = "all"; HUB_START_VIEW = "all"; HUB_SHOT_GHOST_OPEN = "3" }   # pack index loaded (needs network): Banjo-Tooie is not in it
Shot "ghost_installable" @{ HUB_SHOT_FAKE_LIB = "all"; HUB_START_VIEW = "all"; HUB_SHOT_GHOST_OPEN = "3"; HUB_SHOT_GHOST_INSTALLABLE = "1" }   # pack "has" the game: Install enabled, so it is the default
