# Controller hand-off QA (Plan 4 P4, experimental, rollback.h kPktHandoff): headless + muted, LOCAL server only (qa\_guard.ps1).
# 2 players (NES netplay ROM, default) or 3 (SNES multitap ROM) play a rollback game with --handoff; the runner_ctl "handoff" command (retrorun_ctl_send, the hub's own sender)
# passes controller ports between the PCs mid-game and back again. RETRORUN_FUZZ_SLOT=1 makes the fuzz pad depend on WHO presses it, so a hand-off visibly changes the game.
# Pass = every PC exits 0, no DESYNC, identical output/input hash chains on every PC, every PC logged the same hand-offs at the same frames, the refusals answered, and the
# result differs from the control run without hand-offs (the ownership change really reached the game). Needs the retrorun_ctl_send target.
# Usage: powershell -File qa\handoff.ps1 [-Server ws://127.0.0.1:8761] [-Players 2|3] [-Frames 2400] [-Relay] [-Resync] [-NoControl] [-Tag handoff]
param(
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Build = 'build',
    [int]$Players = 2,
    [int]$Frames = 2400,
    [switch]$Relay,        # HUBNET_FORCE_RELAY=1
    [switch]$Resync,       # leave desync recovery on (default off: a desync must be fatal and visible)
    [switch]$NoControl,    # skip the control run without hand-offs
    [string]$Tag = ''
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if ($Tag -eq '') { $Tag = "handoff$Players" + $(if ($Relay) { 'r' } else { '' }) }
$exe = Join-Path $root ($Build + '\bin\Release\retrorun.exe')
$ctlSend = Join-Path $root ($Build + '\bin\Release\retrorun_ctl_send.exe')
if (-not (Test-Path $ctlSend)) { Write-Output "FAIL: $ctlSend missing (build the retrorun_ctl_send target)"; exit 1 }
if (-not (Test-Path (Join-Path $root 'deps\testroms\netplay.nes'))) { python "$PSScriptRoot\make_nesnes_roms.py" (Join-Path $root 'deps\testroms') }
if ($Players -eq 2) { $core = 'nestopia_libretro.dll'; $rom = 'deps\testroms\netplay.nes'; $sys = 'nes'; $ports = '0,1,-1,-1' }
else { $core = 'snes9x_libretro.dll'; $rom = 'deps\testroms\netplay.sfc'; $sys = 'snes'; $ports = '0,1,2,-1' }
$env:RETRORUN_MUTE = '1'
$env:RETRORUN_FUZZ_SLOT = '1'
if ($Resync) { Remove-Item Env:RETRORUN_NO_RESYNC -ErrorAction SilentlyContinue } else { $env:RETRORUN_NO_RESYNC = '1' }
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random
$fail = 0

function Ctl($peer, [string[]]$a) {   # one command through the hub's sender; returns the ack (or $null)
    $o = @(& $ctlSend $peer.Ctl @a)
    $seq = [long]($o | Select-Object -First 1)
    if ($seq -le 0) { return $null }
    $t = Get-Date
    while (((Get-Date) - $t).TotalSeconds -lt 10) {
        try { $j = Get-Content "$($peer.Run)\control_ack.json" -Raw | ConvertFrom-Json; if ($j.seq -eq $seq) { return $j } } catch {}
        Start-Sleep -Milliseconds 150
    }
    return $null
}

function Run-Scenario([string]$name, [bool]$withHandoffs) {
    $out = Join-Path $root ('qa\out\' + $Tag + '_' + $name)
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
    $peers = @()
    foreach ($slot in 0..($Players - 1)) {
        $run = "$out\run$slot"; $log = "$out\slot$slot.log"
        New-Item -ItemType Directory -Force -Path "$out\sv$slot", "$out\st$slot", $run | Out-Null
        '{"seq":0}' | Set-Content "$run\control.json" -Encoding ascii
        $a = @('--core', (Join-Path $root "deps\cores\$core"), '--rom', (Join-Path $root $rom), '--system', $sys, '--save-dir', "$out\sv$slot", '--state-dir', "$out\st$slot",
            '--headless', '--frames', $Frames, '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $slot, '--players', $Players, '--ports', $ports, '--rollback',
            '--delay', 'auto', '--handoff', '--control', "$run\control.json", '--game-key', 'qa-handoff', '--log', $log)
        $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden
        $peers += [pscustomobject]@{ Slot = $slot; Proc = $p; Ctl = "$run\control.json"; Run = $run; Log = $log }
        Start-Sleep -Milliseconds 150
    }
    Write-Host "[$name] room=$room players=$Players frames=$Frames handoffs=$withHandoffs"
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 60) {   # everybody is playing
        $ready = @($peers | Where-Object { (Test-Path $_.Log) -and (Select-String -Path $_.Log -Pattern 'players ready, starting' -Quiet) }).Count
        if ($ready -eq $peers.Count) { break }
        Start-Sleep -Milliseconds 300
    }
    if ($withHandoffs) {
        Start-Sleep -Seconds 5
        $r = Ctl $peers[1] @('handoff', 'slot=0', 'value=0')   # slot 1 does not own port 0: refused at once
        if ($r -and -not $r.ok) { Write-Host "ok[$name]: a non-owner is refused ('$($r.msg)')" } else { Write-Host "FAIL[$name]: a non-owner's request was not refused ($($r | ConvertTo-Json -Compress))"; $script:fail++ }
        if ($Players -eq 2) {
            $steps = @(@(0, 'handoff', 'slot=0', 'value=1'), @(1, 'handoff', 'slot=0', 'value=0'))
        } else {
            $steps = @(@(0, 'handoff', 'slot=0', 'value=2'), @(1, 'handoff', 'slot=1', 'value=2'), @(2, 'handoff', 'slot=0', 'value=0'), @(2, 'handoff', 'slot=1', 'value=1'))
        }
        foreach ($st in $steps) {
            $r = Ctl $peers[$st[0]] ($st[1..3])
            if ($r -and $r.ok) { Write-Host "ok[$name]: slot $($st[0]) $($st[1..3] -join ' ') -> '$($r.msg)'" } else { Write-Host "FAIL[$name]: slot $($st[0]) $($st[1..3] -join ' ') -> $($r | ConvertTo-Json -Compress)"; $script:fail++ }
            Start-Sleep -Seconds 6
        }
    }
    $t1 = Get-Date
    while (@($peers | Where-Object { -not $_.Proc.HasExited }).Count -gt 0) {
        if (((Get-Date) - $t1).TotalSeconds -gt ($Frames / 60 + 120)) { foreach ($pp in $peers) { if (-not $pp.Proc.HasExited) { Stop-Process -Id $pp.Proc.Id -Force; Write-Host "HARD TIMEOUT slot $($pp.Slot)" } }; $script:fail++; break }
        Start-Sleep -Seconds 1
    }
    $res = [ordered]@{ chains = @(); handoffs = @() }
    foreach ($pp in $peers) {
        $lines = if (Test-Path $pp.Log) { Get-Content $pp.Log } else { @() }
        $final = $lines | Where-Object { $_ -match 'final:' } | Select-Object -Last 1
        $desync = @($lines | Where-Object { $_ -match 'DESYNC' }).Count
        $rc = $pp.Proc.ExitCode
        $ho = @($lines | Where-Object { $_ -match 'handoff: port [0-9] slot' } | ForEach-Object { ($_ -replace '^.*handoff: ', '') -replace ' \(we are at.*$', '' })
        Write-Host ("[$name] slot $($pp.Slot) rc=$rc | " + $(if ($final) { $final.Substring($final.IndexOf('final:')) } else { 'NO FINAL' }) + " | hand-offs: $($ho.Count)")
        if ($rc -ne 0 -or -not $final -or $desync) { $script:fail++; Write-Host "   FAIL slot $($pp.Slot): rc=$rc final=$([bool]$final) desync=$desync" }
        if ($final) { $res.chains += (($final -replace '.*final: ', '') -replace 'state-hash \w+ ', '') }
        $res.handoffs += , ($ho -join ' ; ')
    }
    $u = @($res.chains | Select-Object -Unique)
    if ($u.Count -ne 1 -or $res.chains.Count -ne $Players) { $script:fail++; Write-Host "FAIL[$name]: the hash chains differ between the PCs: $($u -join ' || ')" } else { Write-Host "ok[$name]: identical (frame, output, input) chain on every PC: $($u[0])" }
    if ($withHandoffs) {
        $hu = @($res.handoffs | Select-Object -Unique)
        $expected = if ($Players -eq 2) { 2 } else { 4 }
        $cnt = ($res.handoffs[0] -split ' ; ').Count
        if ($hu.Count -ne 1 -or $cnt -ne $expected) { $script:fail++; Write-Host "FAIL[$name]: the PCs did not log the same $expected hand-offs: $($hu -join ' || ')" } else { Write-Host "ok[$name]: every PC applied the same $expected hand-offs: $($res.handoffs[0])" }
    }
    return $u[0]
}

$t = Get-Date
$withChain = Run-Scenario 'handoff' $true
if (-not $NoControl) {
    $ctlChain = Run-Scenario 'control' $false
    if ($withChain -and $ctlChain -and $withChain -eq $ctlChain) { $fail++; Write-Output 'FAIL: the hand-off run ended exactly like the control run (the ownership change never reached the game)' }
    elseif ($withChain -and $ctlChain) { Write-Output 'ok: the hand-off run differs from the control run (the new owner really played the port)' }
}
Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue
Write-Output ("wall=" + [int]((Get-Date) - $t).TotalSeconds + "s")
if ($fail) { Write-Output "HANDOFF QA: FAIL ($fail)"; exit 1 }
Write-Output 'HANDOFF QA: PASS'; exit 0
