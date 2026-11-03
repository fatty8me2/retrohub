# Headless screenshots of the Home Continue hero + "Jump back in" row (fake library, made-up history and thumbnails, procedural covers).
# Scratch vault + scratch userdata history, muted, SDL dummy driver: no window is ever shown, no real saves/screenshots are read.
# Usage: powershell -NoProfile -File qa\continue_shots.ps1 [outdir] [sizes]     (BMPs are converted to PNG; trace.txt has the hero lines)
param([string]$Out = "$env:TEMP\continue_shots", [string]$Sizes = '1280x800,1600x1000')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\bin\Release\RetroHub.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$trace = Join-Path $Out 'trace.txt'
Remove-Item $trace -ErrorAction SilentlyContinue
$ud = Join-Path $root 'build\bin\Release\userdata'

function Shot([string]$name, [string]$size, [hashtable]$extra) {
  $bmp = Join-Path $Out "$name-$size.bmp"
  Remove-Item $bmp -ErrorAction SilentlyContinue
  $vault = Join-Path $Out "vault-$name"
  Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
  foreach ($f in 'hub.json', 'library.json') { Remove-Item (Join-Path $ud $f) -ErrorAction SilentlyContinue }   # scratch play history of earlier runs
  $set = @{ RETROHUB_NO_SHELL = '1'; SSB64_MUTE = '1'; RETRORUN_MUTE = '1'; N64RUN_MUTE = '1'; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = $size;
            HUB_SHOT_NOWAIT = '1'; HUB_SHOT_SECS = '3'; HUB_SHOT_FAKE_LIB = '12'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_VAULT = $vault; HUB_SHOT_TRACE = $trace }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  $psi = New-Object System.Diagnostics.ProcessStartInfo $exe
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.WorkingDirectory = Split-Path $exe -Parent
  foreach ($k in $set.Keys) { $psi.EnvironmentVariables[$k] = [string]$set[$k] }
  Add-Content $trace "== $name $size"
  $p = [System.Diagnostics.Process]::Start($psi)
  if (-not $p.WaitForExit(90000)) { $p.Kill() }   # our own PID only
  if (Test-Path $bmp) {
    $img = [System.Drawing.Image]::FromFile($bmp)
    $img.Save((Join-Path $Out "$name-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $bmp
    Write-Host "ok  $name-$size"
  } else { Write-Host "MISSING $name-$size" }
}

foreach ($size in $Sizes.Split(',')) {
  Shot 'continue_state'   $size @{ HUB_SHOT_STATES = '1'; HUB_SHOT_CONTINUE = '1' }       # newest state thumbnail backdrop, Continue
  Shot 'continue_cover'   $size @{ HUB_SHOT_CONTINUE = '1' }                               # history but no state / screenshot: cover backdrop, Play
  Shot 'continue_newuser' $size @{}                                                        # nothing played: today's hero
  Shot 'continue_playing' $size @{ HUB_SHOT_STATES = '1'; HUB_SHOT_CONTINUE = 'playing' }  # "Playing now", buttons disabled
}
