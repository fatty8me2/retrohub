# Headless screenshots of the Cases library using a REAL player's library (isolated copy; the source userdata is only ever read).
# Usage: qa\real_library_shots.ps1 [-DataFrom <userdata dir>] [-Out <dir>] [-Sizes 1600x1000,1280x800] [-Scales 100,150] [-Consoles gc,n64] [-Modes browse,flip,inspect]
#   -DataFrom  a userdata folder (hub.json, library_cache.json, art\, ...). Default: the QA snapshot of the real library.
#   -Out       where the PNGs + contact sheets go (default qa_out\cases-default under the repo).
#   -Consoles  limit the console tabs (default: every console present in the library's cache, plus the All tab).
#   -Modes     HUB_SHOT_CASES modes taken for the All tab (default browse,flip,inspect); console tabs get browse, plus "cartridge:<c>" at the first size.
#   -Env       extra env vars for every shot, e.g. @{HUB_SHOT_FAKE_LIB="1"} (one made-up game) or @{HUB_SHOT_FAKE_FRIENDS="1";HUB_SHOT_FAKE_FRIEND_LIBS="40"} (friends' games only, with an empty -DataFrom).  -Tag  prefix for the file names.
#   -Scales    UI scale percentages (100, 150).  -Secs  seconds to run before the shot (default 10, so cached art loads).
# A temp copy of RetroHub.exe + SDL2.dll + data\ + controls\ (from build\bin\Release) and a COPY of -DataFrom (rom-cache is not copied) is made;
# every run is hidden, muted, no network (headless mode), and killed by PID if it overruns. Nothing is written to -DataFrom.
param([string]$DataFrom = "C:\Users\fatty\AppData\Local\Temp\claude\a82a6632bfc5ba1b\7dd1fb5c-5483-47af-a677-8507ac24625b\scratchpad\rh_real\userdata",
      [string]$Out = "", [string[]]$Sizes = @("1600x1000", "1280x800"), [string[]]$Scales = @("100", "150"), [string[]]$Consoles = @(),
      [string[]]$Modes = @("browse", "flip", "inspect"), [int]$Secs = 10, [switch]$NoSheets, [hashtable]$Env = @{}, [string]$Tag = "")
$ErrorActionPreference = "Stop"
$split = { param($a) @($a | ForEach-Object { "$_" -split "," } | Where-Object { $_ }) }
$Sizes = & $split $Sizes; $Scales = & $split $Scales; $Consoles = & $split $Consoles; $Modes = & $split $Modes
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\bin\Release"
if (-not $Out) { $Out = Join-Path $root "qa_out\cases-default" }
# RetroHub runs from a temp copy, so a relative -Out would land (and vanish) inside that copy: make it absolute against the caller's folder
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
if (-not (Test-Path (Join-Path $DataFrom "hub.json")) -and -not (Test-Path (Join-Path $DataFrom "library_cache.json"))) { if (-not (Test-Path $DataFrom)) { throw "DataFrom not found: $DataFrom" } }
New-Item -ItemType Directory -Force $Out | Out-Null

# ---- isolated copy ----
$run = Join-Path $env:TEMP ("rh_real_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $run | Out-Null
Copy-Item (Join-Path $bin "RetroHub.exe"), (Join-Path $bin "SDL2.dll") $run
foreach ($d in "data", "controls") { if (Test-Path (Join-Path $bin $d)) { Copy-Item (Join-Path $bin $d) (Join-Path $run $d) -Recurse } }
$ud = Join-Path $run "userdata"
& robocopy $DataFrom $ud /E /XD rom-cache logs /NFL /NDL /NJH /NJS /NP | Out-Null
Write-Host "isolated copy: $run"

# ---- which console tabs does this library have? (system field of the library cache; "gamepack" folders etc. are covered by the entries) ----
$present = @()
$cache = Join-Path $DataFrom "library_cache.json"
if (Test-Path $cache) { $present = @([regex]::Matches((Get-Content $cache -Raw), '"sys":"([a-z0-9]+)"') | ForEach-Object { $_.Groups[1].Value } | Group-Object | Sort-Object Count -Descending | ForEach-Object { $_.Name }) }
if ($Consoles.Count -eq 0) { $Consoles = $present }
Write-Host ("consoles: " + ($Consoles -join " "))

$exe = Join-Path $run "RetroHub.exe"
Add-Type -AssemblyName System.Drawing
function Shot([string]$name, [string]$size, [string]$scale, [hashtable]$extra) {
  $name = "$Tag$name" -replace '[:\\/*?"<>|]', '-'
  $ui = if ($scale -ne "100") { "-ui$scale" } else { "" }
  $bmp = Join-Path $Out "$name-$size$ui.bmp"
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "$Secs" }
  if ($scale -ne "100") { $set["HUB_UI_SCALE"] = $scale }
  foreach ($k in $Env.Keys) { $set[$k] = $Env[$k] }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  try {
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $p = Start-Process $exe -WorkingDirectory $run -PassThru -WindowStyle Hidden
    try { $p.PriorityClass = 'BelowNormal' } catch {}
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force }
  } finally { foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
  if (Test-Path $bmp) {
    $i = [System.Drawing.Image]::FromFile($bmp); $i.Save(($bmp -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $bmp
    Write-Host "shot $name $size$ui"
  } else { Write-Host "NO SHOT: $name $size$ui" }
}

foreach ($scale in $Scales) {
  foreach ($size in $Sizes) {
    foreach ($m in $Modes) { Shot "all-$m" $size $scale @{ HUB_SHOT_CASES = $m } }
    foreach ($c in $Consoles) { Shot "$c-browse" $size $scale @{ HUB_SHOT_CASES = "browse"; HUB_START_SYS = $c } }
  }
}
foreach ($c in $Consoles) { Shot "$c-cartridge" $Sizes[0] "100" @{ HUB_SHOT_CASES = "cartridge:$c" } }

# ---- contact sheets (whole frames, 3 across): one per size/scale of the console tabs, one of the All-tab modes ----
if (-not $NoSheets) {
  $sheet = Join-Path $PSScriptRoot "contact_sheet.ps1"
  foreach ($scale in $Scales) { foreach ($size in $Sizes) {
    $ui = if ($scale -ne "100") { "-ui$scale" } else { "" }
    $files = @(@("all-browse") + @($Consoles | ForEach-Object { "$_-browse" }) | ForEach-Object { Join-Path $Out "$Tag$_-$size$ui.png" } | Where-Object { Test-Path $_ })
    if ($files.Count -gt 0) { & $sheet -Cols 3 -Full (Join-Path $Out "sheet-tabs-$size$ui.png") @files }
  } }
  $files = @($Modes | ForEach-Object { Join-Path $Out "${Tag}all-$_-$($Sizes[0]).png" } | Where-Object { Test-Path $_ })
  if ($files.Count -gt 0) { & $sheet -Cols 3 -Full (Join-Path $Out "sheet-modes-$($Sizes[0]).png") @files }
}
Remove-Item $run -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "screenshots in $Out"
