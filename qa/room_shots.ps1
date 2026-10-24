# Headless screenshots of the online room (host/guest, 2-4 members, game or not). Output: $out\*.bmp
param([string]$out = "$PSScriptRoot\..\shots")
New-Item -ItemType Directory -Force $out | Out-Null
$exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe"
$env:RETROHUB_NO_SHELL = "1"; $env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
$cases = @(
  @{n="host3_waiting"; e=@{HUB_SHOT_ROOM_N="3"}},
  @{n="host3_allready"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_READY="1,2"}},
  @{n="host4_partial"; e=@{HUB_SHOT_ROOM_N="4"; HUB_SHOT_READY="1"}},
  @{n="host2_nogame"; e=@{HUB_SHOT_ROOM_N="2"; HUB_SHOT_NOGAME="1"}},
  @{n="guest3_notready"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_HOSTCHANGE="other"}},
  @{n="guest3_ready"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_HOSTCHANGE="other"; HUB_SHOT_READY="me,2"}},
  @{n="guest4_nogame"; e=@{HUB_SHOT_ROOM_N="4"; HUB_SHOT_HOSTCHANGE="other"; HUB_SHOT_NOGAME="1"}},
  @{n="host3_vermismatch"; e=@{HUB_SHOT_ROOM_N="3"; HUB_SHOT_FAKE_APP="1.9.1"}}
)
foreach ($size in @("1280x800", "1600x1000")) {
  foreach ($c in $cases) {
    $keys = @("HUB_SCREENSHOT","HUB_SHOT_SIZE","HUB_SHOT_FAKE_ROOM","HUB_SHOT_ROOM_N","HUB_SHOT_READY","HUB_SHOT_NOGAME","HUB_SHOT_HOSTCHANGE","HUB_SHOT_FAKE_APP")
    foreach ($k in $keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $env:HUB_SHOT_FAKE_ROOM = "1"; $env:HUB_SHOT_SIZE = $size
    $env:HUB_SCREENSHOT = "$out\$($c.n)_$size.bmp"
    foreach ($k in $c.e.Keys) { Set-Item "Env:$k" $c.e[$k] }
    & $exe | Out-Null
  }
}
