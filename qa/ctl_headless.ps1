# Headless check of the hub -> runner control channel (src/common/runner_ctl.h, docs/RUNNER_CTL.md): drives control.json of a
# running retrorun (GBA test ROM) and n64run (if the ROM exists): save_state slot 3, load_state slot 3, screenshot, quit.
# Verifies slot3.state / slot3.png / slot3.json, the screenshot file, and an ack for every seq. Hidden, muted, no visible windows.
# Plan 4 (retrorun): pause freezes the frame counter and resume continues it, osd_corner is acked (and clears), handoff is refused without --handoff, a Quit sent
# while paused still ends the game, and a command written right after a Quit (the hub menu's "input on") is refused by the real sender (retrorun_ctl_send) so the Quit survives.
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

# ---- retrorun, Plan 4 commands (real-time pacing so frames can be counted): pause, osd_corner, handoff, Quit while paused, a command written right after Quit.
# Commands go through retrorun_ctl_send = the hub's own sender (runctl::send_command_ex), so the "an un-acked Quit is never overwritten" rule is the real one.
$ctlSend = Join-Path $Root 'build\bin\Release\retrorun_ctl_send.exe'
function CtlSend($ctl, [string[]]$a) { $o = @(& $ctlSend $ctl @a); return [pscustomobject]@{ rc = $LASTEXITCODE; seq = [long]($o | Select-Object -First 1); seqs = @($o | ForEach-Object { [long]$_ }) } }
function LastFrames($log) {   # newest "fstat: frames N ms T" of a --realtime run
    if (-not (Test-Path $log)) { return $null }
    $m = Select-String -Path $log -Pattern 'fstat: frames (\d+) ms (\d+)' | Select-Object -Last 1
    if (-not $m) { return $null }
    return [pscustomobject]@{ frames = [long]$m.Matches[0].Groups[1].Value; ms = [long]$m.Matches[0].Groups[2].Value; paused = ($m.Line -match 'paused') }
}
if (-not (Test-Path $ctlSend)) { Write-Host "FAIL[retrorun]: $ctlSend missing (build the retrorun_ctl_send target)"; $fail++ } else {
    $work = Join-Path $env:TEMP ('ctl_p4_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force "$work\sv", "$work\st", "$work\run", "$work\shots" | Out-Null
    $ctl = "$work\run\control.json"
    '{"seq":0}' | Set-Content $ctl -Encoding ascii
    $p = Start-Process -FilePath (Join-Path $Root 'build\bin\Release\retrorun.exe') -PassThru -WindowStyle Hidden -ArgumentList @(
        '--core', (Join-Path $Root 'deps\cores\mgba_libretro.dll'), '--rom', (Join-Path $Root 'deps\testroms\arm.gba'), '--system', 'gba',
        '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--realtime', '--frames', '2000000', '--control', $ctl,
        '--game-key', 'qa-gba', '--shot-dir', "$work\shots", '--log', "$work\p4.log")
    Start-Sleep -Seconds 3
    $f0 = LastFrames "$work\p4.log"
    if (-not $f0) { Write-Host 'FAIL[retrorun]: no fstat lines (is --realtime logging?)'; $fail++ }
    # pause: frames stop, the clock goes on, resume continues
    $r = CtlSend $ctl @('pause', 'value=1'); $a = WaitAck $work\run $r.seq 10
    if (-not $a -or -not $a.ok) { Write-Host "FAIL[retrorun]: pause 1 -> $($a | ConvertTo-Json -Compress)"; $fail++ } else { Write-Host "ok[retrorun]: pause 1 -> $($a.msg)" }
    Start-Sleep -Milliseconds 1200
    $f1 = LastFrames "$work\p4.log"; Start-Sleep -Milliseconds 1500; $f2 = LastFrames "$work\p4.log"
    if (-not $f1 -or -not $f2 -or $f1.frames -ne $f2.frames -or $f2.ms -le $f1.ms -or -not $f2.paused) { Write-Host "FAIL[retrorun]: not frozen while paused ($($f1.frames) -> $($f2.frames), ms $($f1.ms) -> $($f2.ms))"; $fail++ } else { Write-Host "ok[retrorun]: frozen at frame $($f2.frames) while paused" }
    # the OSD still takes commands while paused; osd_corner works (and clears)
    $r = CtlSend $ctl @('osd_corner', 'slot=1', 'value=0', 'text=ROUND 2'); $a = WaitAck $work\run $r.seq 10
    if (-not $a -or -not $a.ok) { Write-Host "FAIL[retrorun]: osd_corner -> $($a | ConvertTo-Json -Compress)"; $fail++ } else { Write-Host "ok[retrorun]: osd_corner shown while paused -> $($a.msg)" }
    $r = CtlSend $ctl @('osd_corner', 'slot=1', 'value=0', 'text='); $a = WaitAck $work\run $r.seq 10
    if (-not $a -or -not $a.ok -or $a.msg -ne 'cleared') { Write-Host "FAIL[retrorun]: osd_corner clear -> $($a | ConvertTo-Json -Compress)"; $fail++ } else { Write-Host 'ok[retrorun]: osd_corner cleared' }
    $r = CtlSend $ctl @('pause', 'value=0'); $a = WaitAck $work\run $r.seq 10
    if (-not $a -or -not $a.ok) { Write-Host "FAIL[retrorun]: pause 0 -> $($a | ConvertTo-Json -Compress)"; $fail++ }
    Start-Sleep -Milliseconds 1500
    $f3 = LastFrames "$work\p4.log"
    if (-not $f3 -or $f3.frames -le $f2.frames + 30 -or $f3.paused) { Write-Host "FAIL[retrorun]: did not resume ($($f2.frames) -> $($f3.frames))"; $fail++ } else { Write-Host "ok[retrorun]: resumed ($($f2.frames) -> $($f3.frames))" }
    # hand-off: a runner without --handoff (and offline) refuses
    $r = CtlSend $ctl @('handoff', 'slot=0', 'value=1'); $a = WaitAck $work\run $r.seq 10
    if (-not $a -or $a.ok) { Write-Host "FAIL[retrorun]: handoff should be refused here -> $($a | ConvertTo-Json -Compress)"; $fail++ } else { Write-Host "ok[retrorun]: handoff refused -> $($a.msg)" }
    # Quit while paused, and a command written right after it (the hub's menu sends "input on" after closing): the Quit must survive
    $r = CtlSend $ctl @('pause', 'value=1'); $null = WaitAck $work\run $r.seq 10
    Start-Sleep -Milliseconds 500
    $qs = CtlSend $ctl @('quit', '--', 'input', 'value=1', '--', 'osd_msg', 'text=late')   # written back to back, far inside the runner's 250 ms poll
    $q = [pscustomobject]@{ seq = $qs.seqs[0] }
    $held = Get-Content $ctl -Raw
    if ($q.seq -le 0 -or $qs.seqs.Count -ne 3 -or $qs.seqs[1] -ne 0 -or $qs.seqs[2] -ne 0 -or $held -notmatch '"cmd":"quit"') { Write-Host "FAIL[retrorun]: a command overwrote the un-acked Quit (seqs $($qs.seqs -join ','), file $held)"; $fail++ } else { Write-Host "ok[retrorun]: commands written right after Quit are refused by the sender (the Quit stays in the file)" }
    $a = WaitAck $work\run $q.seq 15
    if (-not $a -or -not $a.ok) { Write-Host "FAIL[retrorun]: Quit while paused got no ack"; $fail++ } else { Write-Host "ok[retrorun]: Quit while paused -> $($a.msg)" }
    if (-not $p.WaitForExit(20000)) { Write-Host 'FAIL[retrorun]: runner did not quit while paused'; Stop-Process -Id $p.Id; $fail++ } elseif ($p.ExitCode -ne 0) { Write-Host "FAIL[retrorun]: exit code $($p.ExitCode)"; $fail++ } else { Write-Host 'ok[retrorun]: left while paused (exit 0)' }
    $again = CtlSend $ctl @('input', 'value=1')   # acked now: the file is free again
    if ($again.seq -le $q.seq) { Write-Host "FAIL[retrorun]: sender still refuses after the Quit was acked ($($again.seq))"; $fail++ } else { Write-Host 'ok[retrorun]: commands flow again once the Quit is acked' }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

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
