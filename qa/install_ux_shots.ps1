# Headless, muted, hidden screenshots of the whole pack-install flow against the local fake Drive (no real network, no visible window).
# Usage: powershell -NoProfile -File qa\install_ux_shots.ps1 [outdir]      (default qa\shots\install-ux)
param([string]$Out = "$PSScriptRoot\shots\install-ux")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
$data = Join-Path $root "build\bin\Release\userdata"
New-Item -ItemType Directory -Force $Out | Out-Null
$w = Join-Path $env:TEMP "install_ux_shots"
Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $w | Out-Null
python (Join-Path $root "qa\make_pack_fixture.py") "$w\pack" | Out-Null
$port = 18772
$srv = Start-Process python -ArgumentList @((Join-Path $root "qa\gamepack_fake_server.py"), $port, "--packdir", "$w\pack") -PassThru -WindowStyle Hidden
Start-Sleep 2
function Fixture($mb) { $env:PACK_FIXTURE_NOISE_MB = "$mb"; python (Join-Path $root "qa\make_pack_fixture.py") "$w\pack" | Out-Null; Remove-Item env:PACK_FIXTURE_NOISE_MB }
function Mode($m) { Invoke-WebRequest "http://127.0.0.1:$port/__mode?dl=$m" -UseBasicParsing | Out-Null }
function Shot($name, $extra, $secs) {
  $set = @{ RETROHUB_NO_SHELL = "1"; RETROHUB_QA = "1"; RETROHUB_GP_BASE = "127.0.0.1:$port"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"
            HUB_SCREENSHOT = (Join-Path $Out "$name.bmp"); HUB_SHOT_SIZE = "1400x860"; HUB_SHOT_SECS = "$secs"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_START_VIEW = "all"; HUB_START_SYS = "gc" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(120000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name"
}
function Fresh { Remove-Item -Recurse -Force (Join-Path $data "gamepack"), (Join-Path $data "gamepack-tmp"), (Join-Path $data "rom-cache") -ErrorAction SilentlyContinue }
try {
  Mode "ok"; Fresh; Fixture 4
  Shot "01_before_library" @{} 20
  Shot "02_before_details_size" @{ HUB_SHOT_GHOST_PACK = "Gauntlet" } 24
  Mode "slow"; Fresh
  Shot "03_installing_tiles_and_pill" @{ HUB_SHOT_GP_ENQUEUE = "Gauntlet;Pikmin;Fakeland Zip" } 17
  Mode "slow"; Fresh
  Shot "04_installing_details_open" @{ HUB_SHOT_GHOST_PACK = "Gauntlet"; HUB_SHOT_GP_ENQUEUE = "Gauntlet;Pikmin" } 17
  Mode "ok"; Fresh
  Shot "05_after_details_switched_and_toast" @{ HUB_SHOT_GHOST_PACK = "Gauntlet"; HUB_SHOT_GP_ENQUEUE = "Gauntlet" } 24
  Shot "06_after_library_in_place" @{ HUB_SHOT_GP_ENQUEUE = "Gauntlet;Pikmin" } 26
  Shot "07_second_run_installed_persists" @{} 20
  Mode "quota"; Fresh
  Shot "08_error_quota_details" @{ HUB_SHOT_GHOST_PACK = "Gauntlet"; HUB_SHOT_GP_ENQUEUE = "Gauntlet" } 22
  Mode "ok"; Fresh
} finally { $srv.Kill() }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $_.FullName }
Write-Host "screenshots in $Out"
