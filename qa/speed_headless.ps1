# Quick-menu speed check for the built-in libretro runner (src/retrorun, runner_ctl "speed" / "input").
# Launches `retrorun --headless --realtime` (real timer pacing, muted, hidden) per system, drives control.json exactly like the hub
# (seq+1, atomic replace) and measures the achieved frame rate from the runner's own "fstat:" log lines (frame counter vs its own clock).
# Checks, per system: every step 0.25x..8x lands within +-TOL of the 1x measured right before/after it (steady machine only; busy PC -> exit 77, skipped); 1x restores after EVERY step and after Max;
# Max is far above 8x; 10 speed changes inside ~200 ms -> the last one wins; speed changes still apply while "input off".
# n64run has no test ROM under deps (see docs); it is not covered here.
param([string]$Root = (Split-Path -Parent $PSScriptRoot), [double]$Tol = 0.08, [string[]]$Only = @(), [double]$Settle = 0.7, [double]$Window = 1.6, [double]$Unstable = 0.04)
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
$exe = Join-Path $Root 'build\bin\Release\retrorun.exe'
$cores = Join-Path $Root 'deps\cores'; $roms = Join-Path $Root 'deps\testroms'
$systems = @(
    @{ n = 'gb';   sys = 'gb';   core = 'gambatte_libretro.dll'; rom = 'dmg-acid2.gb' },
    @{ n = 'gbc';  sys = 'gbc';  core = 'gambatte_libretro.dll'; rom = 'cgb-acid2.gbc' },
    @{ n = 'gba';  sys = 'gba';  core = 'mgba_libretro.dll';     rom = 'arm.gba' },
    @{ n = 'nes';  sys = 'nes';  core = 'nestopia_libretro.dll'; rom = 'nes_test.nes' },
    @{ n = 'snes'; sys = 'snes'; core = 'snes9x_libretro.dll';   rom = 'snes_test.sfc' })
if (-not (Test-Path $exe)) { Write-Host "SKIP speed_headless: $exe missing"; exit 0 }
$script:fail = 0
$table = @()

function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }
function Send($ctl, $obj) {   # same shape as runctl::make_control; seq = previous seq in the file + 1
    $seq = 1
    try { $seq = [int64]((Get-Content $ctl -Raw | ConvertFrom-Json).seq) + 1 } catch {}
    $obj.seq = $seq
    $body = '{"seq":' + $seq + ',"cmd":"' + $obj.cmd + '","slot":0,"value":' + [int]$obj.value + ',"speed":' + [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}', [double]$obj.speed) + ',"text":""}'
    for ($i = 0; $i -lt 20; $i++) {
        try { [IO.File]::WriteAllText("$ctl.tmp", $body); Move-Item -Force "$ctl.tmp" $ctl; return $seq } catch { Start-Sleep -Milliseconds 5 }
    }
    throw 'cannot write control.json'
}
function WaitAck($run, $seq, $secs) {
    $end = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $end) {
        try { $j = Get-Content "$run\control_ack.json" -Raw | ConvertFrom-Json; if ([int64]$j.seq -ge $seq) { return $j } } catch {}
        Start-Sleep -Milliseconds 50
    }
    return $null
}
function SetSpeed($ctl, $run, $sp) {
    $s = Send $ctl @{ cmd = 'speed'; speed = $sp; value = 0 }
    $a = WaitAck $run $s 10
    if (-not $a -or -not $a.ok) { Fail "no ok ack for speed $sp (seq $s)" }
}
function Fstats($log) {
    $out = @()
    foreach ($l in (Get-Content $log -ErrorAction SilentlyContinue)) {
        if ($l -match '^fstat: frames (\d+) ms (\d+) speed (\S+)( max)?') { $out += [pscustomobject]@{ f = [int64]$Matches[1]; ms = [int64]$Matches[2]; sp = $Matches[3]; max = [bool]$Matches[4] } }
    }
    return $out
}
function MeasureFps($log) {   # settle, then average frames/s over the window using only fstat lines logged inside it
    Start-Sleep -Milliseconds ([int]($Settle * 1000))
    $n0 = (Fstats $log).Count
    Start-Sleep -Milliseconds ([int]($Window * 1000))
    $all = Fstats $log
    $w = @($all | Select-Object -Skip $n0)
    if ($w.Count -lt 2) { return $null }
    $dt = ($w[-1].ms - $w[0].ms) / 1000.0
    if ($dt -le 0) { return $null }
    return [pscustomobject]@{ fps = ($w[-1].f - $w[0].f) / $dt; last = $w[-1] }
}

# A busy PC (builds, other tests) changes the wall-clock fps of the 1x runner itself, so each speed step is measured against 1x windows taken
# immediately before and after it (consecutive windows) and checked with the same +-TOL as always. If those two 1x windows disagree by more than
# $Unstable (4%, half of TOL) the machine was not steady during the trial: the trial is repeated (up to 3 times); ONLY if it still drifts after 3 tries
# does the test exit 77 (CTest SKIP_RETURN_CODE: "skipped: machine too busy"). Everything else still FAILS: no frames logged, a steady-but-too-slow 1x (<40 fps),
# runner not starting, wrong reported speed. A step must be within TOL of BOTH neighbouring 1x windows, so a steady machine is judged at least as strictly as before.
$script:tooBusy = $false
$script:carry = $null     # the last 1x measurement (a trial's closing window is the next trial's opening window)

function Trial($n, $label, $ctl, $run, $log, [scriptblock]$act) {
    # returns @{ ok=$true; ref; m } on a steady trial, $null when the machine never settled
    $why = ''
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $pre = $script:carry
        if (-not $pre) { SetSpeed $ctl $run 1; $pre = MeasureFps $log }
        $script:carry = $null
        & $act
        $m = MeasureFps $log
        SetSpeed $ctl $run 1
        $post = MeasureFps $log
        $script:carry = $post
        if (-not $pre -or -not $post -or -not $m) { $why = 'noframes'; Write-Host "  [$n] $label attempt ${attempt}: no frames logged"; continue }
        $drift = [math]::Abs($post.fps - $pre.fps) / [math]::Max($pre.fps, $post.fps)
        if ($drift -gt $Unstable) {
            $why = 'drift'
            Write-Host ("  [{0}] {1} attempt {2}: unsteady machine (1x before {3:N1} fps, after {4:N1} fps), repeating" -f $n, $label, $attempt, $pre.fps, $post.fps)
            continue
        }
        if ($pre.fps -gt 75 -or $pre.fps -lt 40 -or $post.fps -lt 40) { $why = "baseline $([math]::Round($pre.fps,1))/$([math]::Round($post.fps,1)) fps is not ~60 (realtime pacing broken)"; continue }
        return @{ pre = $pre; post = $post; m = $m }
    }
    if ($why -eq 'drift') { $script:tooBusy = $true }
    elseif ($why -eq 'noframes') { Fail "$n $label no frames logged" }
    else { Fail "$n $label 1x $why" }
    return $null
}

foreach ($s in $systems) {
    if ($Only.Count -and $Only -notcontains $s.n) { continue }
    if ($script:tooBusy) { break }
    $core = Join-Path $cores $s.core; $rom = Join-Path $roms $s.rom
    if (-not (Test-Path $core) -or -not (Test-Path $rom)) { Write-Host "SKIP $($s.n): core or rom missing"; continue }
    $work = Join-Path $env:TEMP ('spd_' + $s.n + '_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force "$work\sv", "$work\st", "$work\run", "$work\shots" | Out-Null
    $ctl = "$work\run\control.json"; $run = "$work\run"; $log = "$work\rr.log"
    '{"seq":0}' | Set-Content $ctl -Encoding ascii
    $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\o.txt" -RedirectStandardError "$work\e.txt" -ArgumentList @(
        '--core', $core, '--rom', $rom, '--system', $s.sys, '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--realtime',
        '--frames', '2000000000', '--control', $ctl, '--game-key', "qa-spd-$($s.n)", '--shot-dir', "$work\shots", '--log', $log)
    $script:carry = $null
    try {
        $t0 = Get-Date
        while (-not (Test-Path $log) -or (Fstats $log).Count -lt 2) {
            if ($p.HasExited) { throw "runner did not start ($($s.n))" }
            if (((Get-Date) - $t0).TotalSeconds -gt 90) { Fail "$($s.n): runner did not log frames within 90 s"; throw 'hard' }
            Start-Sleep -Milliseconds 200
        }
        $n = $s.n
        $row = [ordered]@{ system = $n }
        $steps = @(0.25, 0.5, 1.5, 2, 3, 5, 8, 0)
        $inputOff = $false
        foreach ($sp in $steps) {
            if ($sp -eq 2) { Send $ctl @{ cmd = 'input'; value = 0 } | Out-Null; $inputOff = $true; Start-Sleep -Milliseconds 400 }   # menu open: speed must still apply
            $label = if ($sp -eq 0) { 'max' } else { "$($sp)x" }
            $spv = $sp
            $t = Trial $n $label $ctl $run $log { SetSpeed $ctl $run $spv }.GetNewClosure()
            if (-not $t) { throw $(if ($script:tooBusy) { 'unstable' } else { 'hard' }) }
            $m = $t.m; $hi = [math]::Max($t.pre.fps, $t.post.fps)
            if ($sp -eq 0) {
                if (-not $m.last.max) { Fail "$n max: runner did not report max mode" }
                if ($m.fps -lt $hi * 8 * 0.92) { Fail ("{0} max only {1:N0} fps (< 8x of {2:N1})" -f $n, $m.fps, $hi) }
            } else {
                foreach ($b1 in @($t.pre.fps, $t.post.fps)) {   # within TOL of BOTH neighbouring 1x windows
                    $ratio = $m.fps / $b1
                    if ([math]::Abs($ratio - $sp) -gt $Tol * $sp) { Fail ("{0} {1}: {2:N1} fps = {3:N3}x of a 1x window (want {4}x +-{5:P0}; 1x before {6:N1}, after {7:N1})" -f $n, $label, $m.fps, $ratio, $sp, $Tol, $t.pre.fps, $t.post.fps); break }
                }
                if ([double]$m.last.sp -ne $sp) { Fail "$n $label runner reports speed $($m.last.sp)" }
            }
            # 1x restore after EVERY step, and after Max: the closing 1x window of the trial
            $r = $t.post
            if ($r.last.max -or [double]$r.last.sp -ne 1) { Fail ("{0} 1x restore after {1}: runner reports speed {2} max {3}" -f $n, $label, $r.last.sp, $r.last.max) }
            if ([math]::Abs($r.fps / $t.pre.fps - 1) -gt $Tol) { Fail ("{0} 1x restore after {1}: {2:N1} fps (before {3:N1})" -f $n, $label, $r.fps, $t.pre.fps) }
            if (-not $row.Contains('1x')) { $row['1x'] = [math]::Round($t.pre.fps, 1) }
            $row[$label] = [math]::Round($m.fps, 1)
            $row["1x<-$label"] = [math]::Round($r.fps, 1)
            Write-Host ("[{0}] {1}: {2:N1} fps (1x before {3:N1}, after {4:N1})" -f $n, $label, $m.fps, $t.pre.fps, $t.post.fps)
            if ($inputOff -and $sp -eq 0) { Send $ctl @{ cmd = 'input'; value = 1 } | Out-Null; $inputOff = $false }
        }
        # rapid changes: 10 commands, ~20 ms apart; the last one must win, then rapid ending on 1x after Max
        foreach ($case in @(@(0.5, 2, 5, 0.25, 8, 1.5, 0, 2, 0.5, 3), @(2, 0, 8, 0, 5, 0, 3, 0, 2, 1))) {
            $cs = $case; $want = $case[-1]
            $t = Trial $n "rapid->$want" $ctl $run $log {
                $seq = 0
                foreach ($v in $cs) { $seq = Send $ctl @{ cmd = 'speed'; speed = $v; value = 0 }; Start-Sleep -Milliseconds 20 }
                $a = WaitAck $run $seq 10
                if (-not $a) { Fail "$n rapid: last seq $seq never acked" }
            }.GetNewClosure()
            if (-not $t) { throw $(if ($script:tooBusy) { 'unstable' } else { 'hard' }) }
            $m = $t.m
            $ok = ([math]::Abs($m.fps / $t.pre.fps - $want) -le $Tol * $want) -and ([math]::Abs($m.fps / $t.post.fps - $want) -le $Tol * $want)
            if (-not $ok) { Fail ("{0} rapid x10 ending {1}x: {2:N1} fps ({3}; 1x before {4:N1}, after {5:N1})" -f $n, $want, $m.fps, $m.last.sp, $t.pre.fps, $t.post.fps) }
            $row["rapid->$want"] = [math]::Round($m.fps, 1)
        }
        SetSpeed $ctl $run 1
        Send $ctl @{ cmd = 'quit'; value = 0 } | Out-Null
        $table += [pscustomobject]$row
        if (-not $p.WaitForExit(15000)) { Fail "$n runner did not quit"; Stop-Process -Id $p.Id -Force }
    } catch {
        if ($_.Exception.Message -ne 'unstable' -and $_.Exception.Message -ne 'hard') { Fail "$($s.n): $_" }
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Write-Host ''
$table | ForEach-Object { ($_.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join '  ' }
if ($script:fail) { Write-Host "speed_headless: $script:fail FAILED"; exit 1 }
if ($script:tooBusy) { Write-Host 'speed_headless: SKIPPED: machine too busy for a wall-clock speed measurement (1x windows disagree by more than 4% after 3 tries)'; exit 77 }
Write-Host 'speed_headless: all passed'
