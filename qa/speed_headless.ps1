# Quick-menu speed check for the built-in libretro runner (src/retrorun, runner_ctl "speed" / "input").
# Launches `retrorun --headless --realtime` (real timer pacing, muted, hidden) per system, drives control.json exactly like the hub
# (seq+1, atomic replace) and measures the achieved frame rate from the runner's own "fstat:" log lines (frame counter vs its own clock).
# Checks, per system: every step 0.25x..8x lands within +-TOL of baseline*speed; 1x restores after EVERY step and after Max;
# Max is far above 8x; 10 speed changes inside ~200 ms -> the last one wins; speed changes still apply while "input off".
# n64run has no test ROM under deps (see docs); it is not covered here.
param([string]$Root = (Split-Path -Parent $PSScriptRoot), [double]$Tol = 0.08, [string[]]$Only = @(), [double]$Settle = 0.7, [double]$Window = 1.6)
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

foreach ($s in $systems) {
    if ($Only.Count -and $Only -notcontains $s.n) { continue }
    $core = Join-Path $cores $s.core; $rom = Join-Path $roms $s.rom
    if (-not (Test-Path $core) -or -not (Test-Path $rom)) { Write-Host "SKIP $($s.n): core or rom missing"; continue }
    $work = Join-Path $env:TEMP ('spd_' + $s.n + '_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force "$work\sv", "$work\st", "$work\run", "$work\shots" | Out-Null
    $ctl = "$work\run\control.json"; $run = "$work\run"; $log = "$work\rr.log"
    '{"seq":0}' | Set-Content $ctl -Encoding ascii
    $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\o.txt" -RedirectStandardError "$work\e.txt" -ArgumentList @(
        '--core', $core, '--rom', $rom, '--system', $s.sys, '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--realtime',
        '--frames', '2000000000', '--control', $ctl, '--game-key', "qa-spd-$($s.n)", '--shot-dir', "$work\shots", '--log', $log)
    try {
        $t0 = Get-Date
        while (-not (Test-Path $log) -or (Fstats $log).Count -lt 2) { if ($p.HasExited -or ((Get-Date) - $t0).TotalSeconds -gt 30) { throw "runner did not start ($($s.n))" }; Start-Sleep -Milliseconds 200 }
        $n = $s.n
        $b = MeasureFps $log
        $base = $b.fps
        Write-Host ("[{0}] baseline 1x = {1:N1} fps" -f $n, $base)
        if ($base -lt 40 -or $base -gt 75) { Fail "$n baseline $base fps is not ~60 (realtime pacing broken)" }
        $row = [ordered]@{ system = $n; '1x' = [math]::Round($base, 1) }
        $steps = @(0.25, 0.5, 1.5, 2, 3, 5, 8, 0)
        $inputOff = $false
        foreach ($sp in $steps) {
            if ($sp -eq 2) { Send $ctl @{ cmd = 'input'; value = 0 } | Out-Null; $inputOff = $true; Start-Sleep -Milliseconds 400 }   # menu open: speed must still apply
            SetSpeed $ctl $run $sp
            $m = MeasureFps $log
            $label = if ($sp -eq 0) { 'max' } else { "$($sp)x" }
            if (-not $m) { Fail "$n $label no frames logged"; continue }
            if ($sp -eq 0) {
                if (-not $m.last.max) { Fail "$n max: runner did not report max mode" }
                if ($m.fps -lt $base * 8 * 0.92) { Fail ("{0} max only {1:N0} fps (< 8x of {2:N1})" -f $n, $m.fps, $base) }
            } else {
                $ratio = $m.fps / $base
                if ([math]::Abs($ratio - $sp) -gt $Tol * $sp) { Fail ("{0} {1}: {2:N1} fps = {3:N3}x (want {4}x +-{5:P0})" -f $n, $label, $m.fps, $ratio, $sp, $Tol) }
                if ([double]$m.last.sp -ne $sp) { Fail "$n $label runner reports speed $($m.last.sp)" }
            }
            $row[$label] = [math]::Round($m.fps, 1)
            SetSpeed $ctl $run 1   # restore to 1x after EVERY step, and after Max
            $r = MeasureFps $log
            if (-not $r -or [math]::Abs($r.fps / $base - 1) -gt $Tol -or $r.last.max -or [double]$r.last.sp -ne 1) { Fail ("{0} 1x restore after {1}: {2:N1} fps (base {3:N1})" -f $n, $label, $r.fps, $base) }
            $row["1x<-$label"] = [math]::Round($r.fps, 1)
            if ($inputOff -and $sp -eq 0) { Send $ctl @{ cmd = 'input'; value = 1 } | Out-Null; $inputOff = $false }
        }
        # rapid changes: 10 commands, ~20 ms apart; the last one (3x) must win, then rapid ending on 1x after Max
        foreach ($case in @(@(0.5, 2, 5, 0.25, 8, 1.5, 0, 2, 0.5, 3), @(2, 0, 8, 0, 5, 0, 3, 0, 2, 1))) {
            $seq = 0
            foreach ($v in $case) { $seq = Send $ctl @{ cmd = 'speed'; speed = $v; value = 0 }; Start-Sleep -Milliseconds 20 }
            $want = $case[-1]
            $a = WaitAck $run $seq 10
            if (-not $a) { Fail "$n rapid: last seq $seq never acked" }
            $m = MeasureFps $log
            $ok = $m -and [math]::Abs($m.fps / $base - $want) -le $Tol * $want
            if (-not $ok) { Fail ("{0} rapid x10 ending {1}x: {2:N1} fps ({3})" -f $n, $want, $m.fps, $m.last.sp) }
            $row["rapid->$want"] = [math]::Round($m.fps, 1)
        }
        SetSpeed $ctl $run 1
        Send $ctl @{ cmd = 'quit'; value = 0 } | Out-Null
        $table += [pscustomobject]$row
        if (-not $p.WaitForExit(15000)) { Fail "$n runner did not quit"; Stop-Process -Id $p.Id -Force }
    } catch {
        Fail "$($s.n): $_"
        if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Write-Host ''
$table | ForEach-Object { ($_.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join '  ' }
if ($script:fail) { Write-Host "speed_headless: $script:fail FAILED"; exit 1 }
Write-Host 'speed_headless: all passed'
