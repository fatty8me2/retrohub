# Party mode (Plan 4 P1-P3) screenshots, HEADLESS (HUB_SCREENSHOT = SDL dummy driver, software renderer: no window is ever shown).
# Output: qa\shots\party\<mode>_<size>.png   modes: panel lobbyguest update playing standings winner vote tie podium couch couchround
# Usage: powershell -NoProfile -File qa\party_shots.ps1 [-Out qa\shots\party] [-Sizes 1280x800,1600x1000] [-Modes panel,standings]
# Also checks every shot rendered something (the file exists and is not a flat colour).
param([string]$Out = "$PSScriptRoot\shots\party", [string[]]$Sizes = @("1280x800", "1600x1000"), [string[]]$Modes = @("panel", "lobbyguest", "update", "playing", "standings", "winner", "vote", "tie", "podium", "couch", "couchround"))
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = "Stop"
$Sizes = @($Sizes | ForEach-Object { $_ -split ',' }); $Modes = @($Modes | ForEach-Object { $_ -split ',' })   # "-File" passes a comma list as one string
New-Item -ItemType Directory -Force $Out | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
$env:RETROHUB_NO_SHELL = "1"; $env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:RETROHUB_QA = "1"
$fail = 0
foreach ($size in $Sizes) {
  foreach ($mode in $Modes) {
    foreach ($k in @("HUB_SCREENSHOT", "HUB_SHOT_SIZE", "HUB_SHOT_PARTY", "HUB_SHOT_PARTY_GUEST", "HUB_SHOT_FAKE_ROOM", "HUB_SHOT_ROOM_N", "HUB_SHOT_PAD", "HUB_SHOT_TRACE", "HUB_SHOT_SECS")) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $bmp = Join-Path $Out "$($mode)_$size.bmp"
    $env:HUB_SHOT_SIZE = $size; $env:HUB_SCREENSHOT = $bmp; $env:HUB_SHOT_PARTY = $mode; $env:HUB_SHOT_SECS = "3"
    if ($mode -eq "standings" -or $mode -eq "playing") { }   # host's view; the guest's view of the same modes is shot below
    & $exe | Out-Null
    if (-not (Test-Path $bmp)) { Write-Output "FAIL $mode $size : no screenshot"; $fail++; continue }
    $img = [System.Drawing.Bitmap]::new($bmp)
    $distinct = New-Object 'System.Collections.Generic.HashSet[int]'
    for ($x = 0; $x -lt $img.Width; $x += 37) { for ($y = 0; $y -lt $img.Height; $y += 29) { [void]$distinct.Add($img.GetPixel($x, $y).ToArgb()) } }
    $png = [IO.Path]::ChangeExtension($bmp, ".png")
    $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose(); Remove-Item $bmp
    if ($distinct.Count -lt 12) { Write-Output "FAIL $mode $size : looks blank ($($distinct.Count) colours)"; $fail++ } else { Write-Output "ok   $mode $size -> $png" }
  }
  # the guest's view of the running night
  foreach ($mode in @("lobbyguest", "playing", "standings", "winner", "vote", "podium")) {
    if ($Modes -notcontains $mode) { continue }
    foreach ($k in @("HUB_SCREENSHOT", "HUB_SHOT_SIZE", "HUB_SHOT_PARTY", "HUB_SHOT_PARTY_GUEST", "HUB_SHOT_SECS")) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $bmp = Join-Path $Out "guest_$($mode)_$size.bmp"
    $env:HUB_SHOT_SIZE = $size; $env:HUB_SCREENSHOT = $bmp; $env:HUB_SHOT_PARTY = $mode; $env:HUB_SHOT_PARTY_GUEST = "1"; $env:HUB_SHOT_SECS = "3"
    & $exe | Out-Null
    if (Test-Path $bmp) {
      $img = [System.Drawing.Bitmap]::new($bmp); $png = [IO.Path]::ChangeExtension($bmp, ".png"); $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose(); Remove-Item $bmp
      Write-Output "ok   guest $mode $size -> $png"
    } else { Write-Output "FAIL guest $mode $size : no screenshot"; $fail++ }
  }
}
if ($fail) { Write-Output "PARTY SHOTS: $fail FAILED"; exit 1 }
Write-Output "PARTY SHOTS: ALL OK"
exit 0
