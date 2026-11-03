# Headless screenshots of the Discover page with a fake library (every 5th recommendation owned) and a fake shared pack
# (the 2nd and 4th of each five installable). No network, muted, no window. Usage: qa\discover_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\discover_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $console, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL="1"; RETROHUB_QA="1"; SSB64_MUTE="1"; RETRORUN_MUTE="1"; N64RUN_MUTE="1"
            HUB_SCREENSHOT=(Join-Path $Out "$name.bmp"); HUB_SHOT_SIZE=$size; HUB_SHOT_SECS="6"
            HUB_SHOT_PAGE="discover:$console"; HUB_SHOT_FAKE_LIB="empty"; HUB_SHOT_DISCOVER_FAKE="1" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name"
}
# Big fake list (HUB_SHOT_DISCOVER_BIG: ~100 made-up picks per console from the real database) + the real gameinfo.tsv catalog.
$big = @{ HUB_SHOT_DISCOVER_BIG="1" }
function V($v) { $h = @{ HUB_SHOT_DISCOVER_BIG="1"; HUB_SHOT_DISCOVER_VIEW=$v }; return $h }
foreach ($sz in @(@("1280","1280x800"), @("1600","1600x1000"))) {
  $w = $sz[0]; $s = $sz[1]
  Shot "default_n64_$w" "n64" $s $big
  Shot "more_n64_$w" "n64" $s (V "more=1")
  Shot "sorted_filtered_$w" "all" $s (V "sort=new;genre=RPG|Action;players=2;decade=1990;status=missing")
  Shot "catalog_snes_$w" "snes" $s (V "catalog")
  Shot "catalog_search_$w" "all" $s (V "catalog;q=mario;sort=title")
  Shot "bottom_show_more_$w" "n64" $s (V "more=1;bottom")
  Shot "bottom_end_offer_$w" "snes" $s (V "genre=Party|Puzzle;bottom")
  Shot "bottom_catalog_more_$w" "gba" $s (V "catalog;more=2;bottom")
  Shot "all_consoles_$w" "all" $s $big
  Shot "empty_$w" "all" $s (V "q=zzzzqq")
  Shot "dropdown_genre_$w" "snes" $s (V "open=genre")
  Shot "dropdown_sort_$w" "snes" $s (V "open=sort")
  Shot "dropdown_status_$w" "gba" $s (V "open=status;friends")
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
