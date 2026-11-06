# Headless screenshots of the Cases navigation: sort chip, sort picker, scrubber marks for different sorts, quick-jump overlay, section bubble.
# HUB_SHOT_CASES = <name>[:<sort key>[+rev][+favs]]   name = browse | sortpicker | quickjump | bubble; sort keys: title last_played most_played recently_added release_year console
# Usage: qa\cases_nav_shots.ps1 [outdir] [-Sizes 1600x1000,1280x800] [-Libs 240,3000]
# Muted, hidden, no network; each run is killed by PID if it overruns.
param([string]$Out = "", [string[]]$Sizes = @("1600x1000", "1280x800"), [string[]]$Libs = @("240", "3000"), [string]$Keys = "", [string]$Art = "posters")
$ErrorActionPreference = "Stop"
$Sizes = @($Sizes | ForEach-Object { $_ -split "," }); $Libs = @($Libs | ForEach-Object { $_ -split "," })
$root = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = Join-Path $root "qa_out\car-nav" }
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $lib, $mode, $keys) {
  $bmp = Join-Path $Out "$name-$lib-$size.bmp"
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp;
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5"; HUB_SHOT_FAKE_LIB = $lib; HUB_SHOT_FAKE_SHELF = "1"; HUB_SHOT_FAKE_AZ = "1"; HUB_SHOT_FAKE_ART = $Art; HUB_SHOT_CASES = $mode }
  if ($keys) { $set["HUB_SHOT_KEYS"] = $keys }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $lib $size"
}
$cases = @(
  @("sort-title", "browse:title"), @("sort-year", "browse:release_year"), @("sort-console", "browse:console"), @("sort-last", "browse:last_played+favs"),
  @("sortpicker", "sortpicker:release_year+rev"), @("quickjump-title", "quickjump:title"), @("quickjump-year", "quickjump:release_year"), @("bubble-title", "bubble:title"), @("bubble-year", "bubble:release_year")
)
foreach ($size in $Sizes) { foreach ($lib in $Libs) { foreach ($c in $cases) { Shot $c[0] $size $lib $c[1] $Keys } } }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $_.FullName }
Write-Host "screenshots in $Out"
