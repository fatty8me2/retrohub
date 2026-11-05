# Headless screenshots of the Cases library layout (HUB_SHOT_CASES = browse | flip | inspect | cartridge[:<console>] | missingart).
# Usage: qa\cases_shots.ps1 [outdir] [-Modes browse,flip] [-Sizes 1600x1000] [-Scale 100]
# Muted, hidden, no network; each run is killed by PID if it overruns.
param([string]$Out = "$env:TEMP\cases_shots", [string[]]$Modes = @("browse", "flip", "inspect", "cartridge", "missingart"), [string[]]$Sizes = @("1280x800", "1600x1000"), [int]$Scale = 100, [string]$Keys = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $bmp = Join-Path $Out "$name-$size.bmp"
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp;
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5"; HUB_SHOT_FAKE_LIB = "240"; HUB_SHOT_FAKE_SHELF = "1"; HUB_SHOT_FAKE_AZ = "1"; HUB_SHOT_FAKE_ART = "1" } + $extra
  if ($Scale -ne 100) { $set["HUB_UI_SCALE"] = "$Scale" }
  if ($Keys) { $set["HUB_SHOT_KEYS"] = $Keys }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in $Sizes) { foreach ($m in $Modes) { Shot "cases-$m$(if ($Scale -ne 100) { "-ui$Scale" })" $size @{ HUB_SHOT_CASES = $m } } }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
