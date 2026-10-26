# Headless screenshots of the room game picker with a large fake library (600 games), host and guest views. Output: $out\*.bmp
# No window, no network, muted. Usage: powershell -File qa\picker_shots.ps1 [-Out dir]
param([string]$out = "$env:TEMP\picker_shots", [int]$Games = 600)
New-Item -ItemType Directory -Force $out | Out-Null
$exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe"
$env:RETROHUB_NO_SHELL = "1"; $env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
$cases = @(
  @{n="host_room"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_PEER_MISSING="6"; HUB_SHOT_ROOM_GAME="fake:3"}},
  @{n="host_picker"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_PEER_MISSING="6"; HUB_SHOT_ROOM_GAME="fake:3"; HUB_SHOT_PICKER_OPEN="1"}},
  @{n="host_search"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_PEER_MISSING="6"; HUB_SHOT_ROOM_GAME="fake:3"; HUB_SHOT_PICKER_OPEN="1"; HUB_SHOT_PICKER_Q="kart"}},
  @{n="host_scrolled"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_PEER_MISSING="6"; HUB_SHOT_ROOM_GAME="fake:3"; HUB_SHOT_PICKER_OPEN="1"; HUB_SHOT_PICKER_SCROLL="4000"}},
  @{n="host_detail"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_PEER_MISSING="6"; HUB_SHOT_ROOM_GAME="fake:3"; HUB_SHOT_PICKER_OPEN="1"; HUB_SHOT_PICKER_DETAIL="fake:7"}},
  @{n="guest_view"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_HOSTCHANGE="other"; HUB_SHOT_ROOM_GAME="crashbash"}}
)
foreach ($size in @("1280x800", "1600x1000")) {
  foreach ($c in $cases) {
    foreach ($k in (Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' }).Name) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $env:HUB_SHOT_FAKE_ROOM = "1"; $env:HUB_SHOT_SIZE = $size; $env:HUB_SHOT_FAKE_LIB = "$Games"; $env:HUB_SHOT_FAKE_TITLES = "1"
    $env:HUB_SHOT_SECS = "4"; $env:HUB_SHOT_NOWAIT = "1"
    $env:HUB_SCREENSHOT = "$out\$($c.n)_$size.bmp"
    foreach ($k in $c.e.Keys) { Set-Item "Env:$k" $c.e[$k] }
    $p = Start-Process $exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { $p.Kill() }
  }
}
foreach ($k in (Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' }).Name) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
Write-Host "shots in $out"
