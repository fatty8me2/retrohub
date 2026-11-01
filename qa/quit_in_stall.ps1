# runners-M1: "Quit to RetroHub" must be heard while the game is blocked waiting for another PC. Headless + muted, LOCAL server only (qa\_guard.ps1).
# Two peers play a netplay game; the guest (slot 1) is frozen with NtSuspendProcess, so the host stalls (rollback: at the prediction limit; lockstep: blocked in the
# input wait). A Quit is then sent to the host through retrorun_ctl_send (the hub's own sender): it must leave within -LeaveSec seconds with exit code 0, well before the
# 10 s "connection lost" timeout, and tell the guest goodbye. The guest is then resumed and must also leave on its own Quit.
# Modes: rollback (NES netplay ROM, --rollback) and lockstep (GBA test ROM). -Relay forces the server relay (HUBNET_FORCE_RELAY=1).
# Usage: powershell -File qa\quit_in_stall.ps1 [-Server ws://127.0.0.1:8761] [-Mode rollback|lockstep|both] [-Relay] [-Exe path\to\retrorun.exe] [-LeaveSec 5]
param([string]$Server = 'ws://127.0.0.1:8761', [string]$Mode = 'both', [switch]$Relay, [string]$Exe = '', [int]$LeaveSec = 5, [string]$Tag = 'quitstall')
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq '') { $Exe = Join-Path $root 'build\bin\Release\retrorun.exe' }
$ctlSend = Join-Path $root 'build\bin\Release\retrorun_ctl_send.exe'
$env:RETRORUN_MUTE = '1'
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }
if (-not (Test-Path (Join-Path $root 'deps\testroms\netplay.nes'))) { python "$PSScriptRoot\make_nesnes_roms.py" (Join-Path $root 'deps\testroms') }
Add-Type -Namespace Rh -Name Nt -MemberDefinition '[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h); [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);'
$fail = 0

function Run-Mode([string]$name, [string[]]$gameArgs, [bool]$rollback) {
    $out = Join-Path $root ('qa\out\' + $Tag + '_' + $name + $(if ($Relay) { '_relay' } else { '' }))
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; $rnd = New-Object System.Random
    $room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
    $peers = @()
    foreach ($slot in 0, 1) {
        $sv = "$out\sv$slot"; $st = "$out\st$slot"; $run = "$out\run$slot"
        New-Item -ItemType Directory -Force -Path $sv, $st, $run | Out-Null
        '{"seq":0}' | Set-Content "$run\control.json" -Encoding ascii
        $a = $gameArgs + @('--save-dir', $sv, '--state-dir', $st, '--headless', '--frames', '2000000', '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $slot,
            '--players', 2, '--ports', '0,1,-1,-1', '--delay', 'auto', '--control', "$run\control.json", '--log', "$out\slot$slot.log")
        if ($rollback) { $a += '--rollback' }
        $p = Start-Process -FilePath $Exe -ArgumentList $a -PassThru -WindowStyle Hidden
        $peers += [pscustomobject]@{ Slot = $slot; Proc = $p; Ctl = "$run\control.json"; Run = $run; Log = "$out\slot$slot.log" }
    }
    $host0 = $peers[0]; $guest = $peers[1]
    # wait until both are really playing (frames are being stepped), then freeze the guest
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 40) {
        if ((Test-Path $host0.Log) -and (Select-String -Path $host0.Log -Pattern 'all 2 players ready|all \d players ready|session ready' -Quiet)) { break }
        Start-Sleep -Milliseconds 300
    }
    Start-Sleep -Seconds 6
    [void][Rh.Nt]::NtSuspendProcess($guest.Proc.Handle)
    Write-Output "[$name] guest frozen; host stalls..."
    Start-Sleep -Seconds 3
    $sent = Get-Date
    $o = @(& $ctlSend $host0.Ctl quit)
    $seq = [long]($o | Select-Object -First 1)
    if ($seq -le 0) { Write-Output "FAIL[$name]: could not send Quit"; $script:fail++ }
    $left = $host0.Proc.WaitForExit($LeaveSec * 1000)
    $took = [math]::Round(((Get-Date) - $sent).TotalSeconds, 1)
    if (-not $left) { Write-Output "FAIL[$name]: the stalled host did not leave within ${LeaveSec}s after Quit"; $script:fail++; try { $host0.Proc.Kill() } catch {} }
    else {
        $rc = $host0.Proc.ExitCode
        $ack = $null
        try { $ack = Get-Content "$($host0.Run)\control_ack.json" -Raw | ConvertFrom-Json } catch {}
        if ($rc -ne 0) { Write-Output "FAIL[$name]: the host exited with $rc (want 0)"; $script:fail++ }
        elseif (-not $ack -or $ack.seq -ne $seq -or -not $ack.ok) { Write-Output "FAIL[$name]: no ok ack for the Quit seq $seq ($($ack | ConvertTo-Json -Compress))"; $script:fail++ }
        else { Write-Output "ok[$name]: the stalled host left $took s after Quit (exit 0, ack '$($ack.msg)')" }
    }
    # the guest wakes up: it must notice the departure and leave on its own Quit (or already have ended)
    [void][Rh.Nt]::NtResumeProcess($guest.Proc.Handle)
    Start-Sleep -Seconds 2
    if (-not $guest.Proc.HasExited) {
        $null = & $ctlSend $guest.Ctl quit
        if (-not $guest.Proc.WaitForExit(15000)) { Write-Output "FAIL[$name]: the guest never left"; $script:fail++; try { $guest.Proc.Kill() } catch {} }
        else { Write-Output "ok[$name]: guest left (exit $($guest.Proc.ExitCode))" }
    } else { Write-Output "ok[$name]: guest ended on its own (exit $($guest.Proc.ExitCode))" }
    foreach ($pp in $peers) { if (-not $pp.Proc.HasExited) { try { $pp.Proc.Kill() } catch {} } }
}

$nes = @('--core', (Join-Path $root 'deps\cores\nestopia_libretro.dll'), '--rom', (Join-Path $root 'deps\testroms\netplay.nes'), '--system', 'nes')
$gba = @('--core', (Join-Path $root 'deps\cores\mgba_libretro.dll'), '--rom', (Join-Path $root 'deps\testroms\sram.gba'), '--system', 'gba')
if ($Mode -in 'rollback', 'both') { Run-Mode 'rollback' $nes $true }
if ($Mode -in 'lockstep', 'both') { Run-Mode 'lockstep' $gba $false }
Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue
if ($fail) { Write-Output "QUIT IN STALL QA: FAIL ($fail)"; exit 1 }
Write-Output 'QUIT IN STALL QA: PASS'; exit 0
