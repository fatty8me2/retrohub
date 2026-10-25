# Play again / invite-last-group / notification history screenshots (headless HUB_SCREENSHOT, fake data, no network).
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
$out = Join-Path $PSScriptRoot "out\group"; New-Item -ItemType Directory -Force $out | Out-Null
$cases = @(
  @{ n = "bell"; env = @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_BELL = "1" } },
  @{ n = "online_home"; env = @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_PAGE = "online" } },
  @{ n = "play_again_host"; env = @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_READY = "1,2"; HUB_SHOT_PLAY_AGAIN = "1" } },
  @{ n = "friend_menu"; env = @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FRIENDS = "1"; HUB_SHOT_CLICK = "" } }
)
foreach ($size in @("1280x800", "1600x1000")) {
  foreach ($c in $cases) {
    $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "3"
              HUB_SHOT_SIZE = $size; HUB_SCREENSHOT = (Join-Path $out "$($c.n)_$size.bmp") }
    foreach ($k in $c.env.Keys) { if ($c.env[$k] -ne "") { $set[$k] = $c.env[$k] } }
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $p = Start-Process $exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force; Write-Host "TIMEOUT $($c.n)" }
    foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    Write-Host ("{0} {1}: {2}" -f $c.n, $size, (Test-Path (Join-Path $out "$($c.n)_$size.bmp")))
  }
}
