# Headless, muted, hidden screenshots of the greyed-out games from the shared Drive packs (reads the real Drive listing only).
# Usage: qa\pack_ghosts_shots.ps1 [outdir]
param([string]$Out = "$PSScriptRoot\shots\pack-ghosts")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name.bmp");
            HUB_SHOT_SIZE = "1600x1000"; HUB_SHOT_SECS = "24"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_START_VIEW = "all" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(120000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name"
}
Shot "all-games" @{}
Shot "all-games-n64" @{ HUB_START_SYS = "n64" }
Shot "pack-only-details" @{ HUB_SHOT_GHOST_PACK = "Mario Golf" }
Shot "pack-only-details-install" @{ HUB_SHOT_GHOST_PACK = "Wind Waker"; HUB_SHOT_GHOST_INSTALLABLE = "1" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $_.FullName }
Write-Host "screenshots in $Out"
