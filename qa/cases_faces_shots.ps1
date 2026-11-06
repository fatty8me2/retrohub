# Headless shots of the Cases carousel on a COPY of a real library (art cache, library cache, game info), one console tab at a time.
# Usage: qa\cases_faces_shots.ps1 -Root <dir with RetroHub.exe + userdata\ copy> [-Out qa_out\cases-faces] [-Consoles nds,gc,...] [-Modes browse,flip,inspect]
#                                 [-Sizes 1600x1000,1280x800] [-Spin "1.2,0"] [-FullCover <png>] [-Prefix real]
# Never visible, muted, no network (headless mode); killed by PID if it overruns. NEVER point -Root at the real data folder.
param([Parameter(Mandatory = $true)][string]$Root, [string]$Out = "qa_out\cases-faces", [string[]]$Consoles = @("nds", "gc", "n64", "ps2", "ps1", "snes", "gba", "gbc", "gb", "3ds", "wiiu", "switch"),
      [string[]]$Modes = @("browse", "flip", "inspect"), [string[]]$Sizes = @("1600x1000"), [string]$Spin = "", [string]$FullCover = "", [string]$Prefix = "real", [int]$Secs = 9)
$ErrorActionPreference = "Stop"
if ($Root -match 'smashClassic\\userdata' -or (Test-Path (Join-Path $Root "..\smashClassic"))) { throw "refusing: not an isolated copy" }
$Consoles = @($Consoles | ForEach-Object { $_ -split "," } | Where-Object { $_ }); $Modes = @($Modes | ForEach-Object { $_ -split "," }); $Sizes = @($Sizes | ForEach-Object { $_ -split "," })
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
foreach ($size in $Sizes) { foreach ($c in $Consoles) { foreach ($m in $Modes) {
  $name = "$Prefix-$c-$m-$size"
  $bmp = Join-Path (Resolve-Path $Out) "$name.bmp"
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "$Secs"; HUB_SHOT_CASES = $m; HUB_START_SYS = $c }
  if ($Spin) { $set["HUB_SHOT_SPIN"] = $Spin }
  if ($FullCover) { $set["HUB_SHOT_FULLCOVER"] = (Resolve-Path $FullCover) }
  try {
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $p = Start-Process (Join-Path $Root "RetroHub.exe") -WorkingDirectory $Root -PassThru -WindowStyle Hidden
    $p.PriorityClass = 'BelowNormal'
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force }
  } finally { foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
  if (Test-Path $bmp) { $i = [System.Drawing.Image]::FromFile($bmp); $i.Save(($bmp -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $bmp; Write-Host "shot $name" } else { Write-Host "NO SHOT $name" }
} } }
