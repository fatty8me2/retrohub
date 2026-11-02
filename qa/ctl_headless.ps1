# Headless check of the hub -> runner control channel (src/common/runner_ctl.h, docs/RUNNER_CTL.md): drives control.json of a
# running retrorun (GBA test ROM) and n64run (if the ROM exists): save_state slot 3, load_state slot 3, screenshot, quit.
# Verifies slot3.state / slot3.png / slot3.json, the screenshot file, and an ack for every seq. Hidden, muted, no visible windows.
param([string]$Root = (Split-Path -Parent $PSScriptRoot),
      [string]$N64Rom = 'C:\Users\fatty\Desktop\Everything\Games\N64\Mario Kart 64 (U) [!].z64')
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
$fail = 0
function Send($ctl, $seq, $cmd, $slot) {
    [IO.File]::WriteAllText($ctl, "{`"seq`":$seq,`"cmd`":`"$cmd`",`"slot`":$slot}")
}
function WaitAck($dir, $seq, $secs) {
    $ack = "$dir\control_ack.json"
    $end = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $end) {
        if (Test-Path $ack) {
            try { $j = Get-Content $ack -Raw | ConvertFrom-Json; if ($j.seq -eq $seq) { return $j } } catch {}
        }
        Start-Sleep -Milliseconds 200
    }
    return $null
}
function Drive($name, $p, $run, $states, $shots, $slotsOk) {
    $ctl = "$run\control.json"
    Start-Sleep -Seconds 4
    $seq = 0
    foreach ($step in @(@('save_state', 3), @('load_state', 3), @('screenshot', 0), @('load_state', 7), @('quit', 0))) {
        $seq++
        Send $ctl $seq $step[0] $step[1]
        $a = WaitAck $run $seq 30
        if (-not $a) { Write-Host "FAIL[$name]: no ack for $($step[0])"; $script:fail++; continue }
        $want = -not ($step[0] -eq 'load_state' -and $step[1] -eq 7)   # empty slot must fail
        if ([bool]$a.ok -ne $want) { Write-Host "FAIL[$name]: $($step[0]) ok=$($a.ok) msg=$($a.msg)"; $script:fail++ } else { Write-Host "ok[$name]: $($step[0]) slot $($step[1]) -> $($a.msg)" }
    }
    foreach ($f in @('slot3.state', 'slot3.png', 'slot3.json')) {
        if (-not (Test-Path "$states\$f")) { Write-Host "FAIL[$name]: missing $f"; $script:fail++ }
    }
    if (-not (Test-Path "$states\auto.state")) { Write-Host "FAIL[$name]: no auto.state after quit"; $script:fail++ }
    $png = Get-ChildItem $shots -Filter *.png -ErrorAction SilentlyContinue
    if (-not $png) { Write-Host "FAIL[$name]: no screenshot in $shots"; $script:fail++ }
    elseif ($png[0].Name -notmatch '^\d{8}-\d{6}\.png$') { Write-Host "FAIL[$name]: bad shot name $($png[0].Name)"; $script:fail++ }
    if (-not $p.WaitForExit(30000)) { Write-Host "FAIL[$name]: runner did not quit"; Stop-Process -Id $p.Id; $script:fail++ }
}

# ---- retrorun (mGBA, GBA test ROM)
$work = Join-Path $env:TEMP ('ctl_rr_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$work\sv", "$work\st", "$work\run", "$work\shots" | Out-Null
'{"seq":0}' | Set-Content "$work\run\control.json" -Encoding ascii
$p = Start-Process -FilePath (Join-Path $Root 'build\bin\Release\retrorun.exe') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\o.txt" -RedirectStandardError "$work\e.txt" -ArgumentList @(
    '--core', (Join-Path $Root 'deps\cores\mgba_libretro.dll'), '--rom', (Join-Path $Root 'deps\testroms\arm.gba'), '--system', 'gba',
    '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--frames', '2000000', '--control', "$work\run\control.json",
    '--game-key', 'qa-gba', '--shot-dir', "$work\shots", '--log', "$work\rr.log")
Drive 'retrorun' $p "$work\run" "$work\st" "$work\shots"
# --load-state at boot
$p2 = Start-Process -FilePath (Join-Path $Root 'build\bin\Release\retrorun.exe') -PassThru -WindowStyle Hidden -ArgumentList @(
    '--core', (Join-Path $Root 'deps\cores\mgba_libretro.dll'), '--rom', (Join-Path $Root 'deps\testroms\arm.gba'), '--system', 'gba',
    '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--frames', '120', '--load-state', "$work\st\slot3.state", '--log', "$work\rr2.log")
$p2.WaitForExit(60000) | Out-Null
if (-not (Select-String -Path "$work\rr2.log" -Pattern 'Resumed from saved state' -Quiet)) { Write-Host 'FAIL[retrorun]: --load-state did not restore'; $fail++ } else { Write-Host 'ok[retrorun]: --load-state' }
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue

# ---- n64run
if (Test-Path -LiteralPath $N64Rom) {
    $work = Join-Path $env:TEMP ('ctl_n64_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force "$work\sv", "$work\st", "$work\run", "$work\shots" | Out-Null
    '{"seq":0}' | Set-Content "$work\run\control.json" -Encoding ascii
    $p = Start-Process -FilePath (Join-Path $Root 'build\bin\Release\n64run.exe') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\o.txt" -RedirectStandardError "$work\e.txt" -ArgumentList @(
        '--rom', "`"$N64Rom`"", '--game', 'qa-ctl', '--headless', '--realtime', '--frames', '0', '--emu-dir', (Join-Path $Root 'deps\emu'),
        '--save-dir', "$work\sv", '--state-dir', "$work\st", '--control', "$work\run\control.json", '--game-key', 'qa-n64', '--shot-dir', "$work\shots")
    Drive 'n64run' $p "$work\run" "$work\st" "$work\shots"
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
} else { Write-Host "SKIP n64run: no ROM at $N64Rom" }

if ($fail) { Write-Host "ctl_headless: $fail FAILED"; exit 1 }
Write-Host 'ctl_headless: all passed'
