# Idle throttle A/B (Plan 4 M3): how many frames RetroHub presents, how often its loop pumps, and how much CPU it uses while it sits behind a
# game, with the throttle off ("before": the old every-iteration-at-vsync loop) and on ("after"). HEADLESS and muted: HUB_SCREENSHOT uses SDL's
# dummy video driver (no window exists), nothing touches the network, the user's install or identity.
#
# HUB_IDLE_SIM=bg,noraster : pretend the hub is in the background, and skip SDL's SOFTWARE rasteriser. In the dummy-driver harness
#   SDL_RenderPresent flushes the whole frame through the software rasteriser (~110 ms a frame whatever the window size), which would
#   drown the loop's own pacing; a GPU build does not pay that. What is left is the hub's real per-frame work (layout, draw lists, the
#   frame hash) plus the 8 ms pacing the screenshot harness adds after every frame, so the numbers show the loop, not the rasteriser.
# HUB_IDLE_THROTTLE=0 is the "before" (the loop exactly as it was); unset is the "after".
# "steady" numbers skip the first 3 s (fonts, library, textures: start-up, the same in both runs).
#
# usage: qa\idle_fps.ps1 [-Build <bin\Release>] [-Secs 12] [-Library 60] [-Size 1280x800]
param(
  [string]$Build = (Join-Path (Split-Path $PSScriptRoot) "build\bin\Release"),
  [int]$Secs = 12, [int]$Library = 60, [string]$Size = "1280x800"
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $Build "RetroHub.exe"
if (-not (Test-Path $exe)) { throw "RetroHub.exe not found in $Build (build the RetroHub target first)" }
$out = Join-Path $PSScriptRoot "out\idle_fps"; New-Item -ItemType Directory -Force $out | Out-Null

function Run-Hub([string]$tag, [bool]$throttle) {
  $report = Join-Path $out "fps_$tag.txt"; Remove-Item $report -ErrorAction SilentlyContinue
  $set = @{ RETROHUB_NO_SHELL = "1"; RETROHUB_NO_UPDATE_CHECK = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"
            HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "$Secs"; HUB_SHOT_SIZE = $Size; HUB_SHOT_FAKE_LIB = "$Library"; HUB_SHOT_FAKE_TITLES = "1"
            HUB_IDLE_SIM = "bg,noraster"; HUB_SCREENSHOT = (Join-Path $out "shot_$tag.bmp"); HUB_FPS_REPORT = $report }
  if (-not $throttle) { $set["HUB_IDLE_THROTTLE"] = "0" }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  try {
    $p = Start-Process $exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(($Secs + 90) * 1000)) { Stop-Process -Id $p.Id -Force; throw "timeout ($tag)" }   # only the pid started here
  } finally { foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
  if (-not (Test-Path $report)) { throw "no report written ($tag)" }
  $line = (Get-Content $report -Raw).Trim()
  Write-Host ("{0,-7} {1}" -f $tag, $line)
  $m = [regex]::Match($line, 'steady \(after 3 s\): secs=([\d.]+) presented_per_s=([\d.]+) loops_per_s=([\d.]+) cpu_pct=([\d.]+)')
  if (-not $m.Success) { throw "unexpected report format ($tag)" }
  [pscustomobject]@{ Tag = $tag; PresentedPerSec = [double]$m.Groups[2].Value; LoopsPerSec = [double]$m.Groups[3].Value; CpuPct = [double]$m.Groups[4].Value }
}

$before = Run-Hub "before" $false
$after = Run-Hub "after" $true
Write-Host ""
Write-Host ("presented frames/s : {0,6:N1} -> {1,6:N1}   ({2:N0}% fewer)" -f $before.PresentedPerSec, $after.PresentedPerSec, (100 * (1 - $after.PresentedPerSec / [math]::Max($before.PresentedPerSec, 0.001))))
Write-Host ("loop iterations/s  : {0,6:N1} -> {1,6:N1}   (every poller keeps running: sockets, drivers, pad watchers)" -f $before.LoopsPerSec, $after.LoopsPerSec)
Write-Host ("CPU of one core    : {0,6:N1}% -> {1,6:N1}%" -f $before.CpuPct, $after.CpuPct)
$fail = 0
if ($after.PresentedPerSec -gt 20) { Write-Host "FAIL: the throttled hub still presents more than 20 frames/s while idle in the background"; $fail++ }
if ($after.LoopsPerSec -lt 55) { Write-Host "FAIL: the loop (the pollers) slowed below ~60 Hz"; $fail++ }
if ($before.PresentedPerSec -lt 40) { Write-Host "FAIL: the unthrottled baseline is slower than expected, the comparison is meaningless"; $fail++ }
if ($fail) { exit 1 } else { Write-Host "OK" }
