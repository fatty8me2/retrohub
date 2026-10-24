# Headless netplay QA for retrorun (Windows PowerShell 5.1).
# Launches N peers (slot 0..N-1) against the live lobby, all hidden, all --headless.
# Usage example:
#   powershell -File qa\retrorun_online.ps1 -Core mgba_libretro.dll -Rom deps\testroms\sram.gba -System gba -Frames 3000
# Optional: -KillSlot 1 -KillAfterSec 15   (kill that peer mid-match; the rest must exit 4, not hang)
# Optional: set $env:HUBNET_FORCE_RELAY=1 before calling to force the WebSocket relay path.
# Sparse slots: -Players is the number of PCs, not the highest slot. e.g. -Slots 0,2 -Ports 0,2,-1,-1 -Players 2.
param(
    [string]$Core = 'mgba_libretro.dll',
    [string]$Rom = 'deps\testroms\sram.gba',
    [string]$Rom2 = '',           # optional: slot 1 uses this ROM instead (desync-detection test)
    [string]$System = 'gba',
    [int]$Frames = 3000,
    [int]$Players = 2,
    [string]$Slots = '',          # optional sparse slot list, e.g. '0,2' (use with -Ports 0,2,-1,-1)
    [string]$Ports = '0,1,-1,-1',
    [string]$ClientDelay = '',   # optional: --delay for every non-host slot (host uses -Delay); the host's D must win
    [string]$Delay = 'auto',   # 'auto' or 0..30
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Room = '',
    [string]$Tag = 'run',
    [int]$KillSlot = -1,
    [int]$KillAfterSec = 15,
    [int]$TimeoutSec = 900,
    [switch]$Rollback,            # --rollback (GGPO-style) instead of lockstep
    [string[]]$Extra = @()        # extra retrorun args, e.g. --link
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$corePath = Join-Path $root ('deps\cores\' + $Core)
$romPath = Join-Path $root $Rom
$outDir = Join-Path $root ('qa\out\' + $Tag)
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }   # stale logs from earlier runs would be re-counted
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$env:RETRORUN_MUTE = '1'

if ($Room -eq '') {
    # Room code from the lobby alphabet A-H J-N P-Z 2-9 (no I, O, 0, 1).
    $alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
    $rnd = New-Object System.Random
    $code = -join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })
    $Room = $code + '-1'
}

$ownerArr = $Ports.Split(',')
$procs = @()
$slotList = if ($Slots -ne '') { @($Slots.Split(',') | ForEach-Object { [int]$_ }) } else { @(0..($Players - 1)) }
foreach ($slot in $slotList) {
    $log = Join-Path $outDir ('slot' + $slot + '.log')
    $sv = Join-Path $outDir ('sv' + $slot)
    $st = Join-Path $outDir ('st' + $slot)
    New-Item -ItemType Directory -Force -Path $sv, $st | Out-Null
    $thisRom = if ($slot -eq 1 -and $Rom2 -ne '') { Join-Path $root $Rom2 } else { $romPath }
    $argList = @('--core', $corePath, '--rom', $thisRom, '--system', $System,
        '--save-dir', $sv, '--state-dir', $st, '--headless', '--frames', $Frames, '--fuzz-input',
        '--net', $Server, '--room', $Room, '--slot', $slot, '--players', $Players,
        '--ports', $Ports, '--delay', $(if ($slot -ne 0 -and $ClientDelay -ne '') { $ClientDelay } else { $Delay }), '--log', $log)
    if ($Rollback) { $argList += '--rollback' }
    $argList += $Extra
    $p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -WindowStyle Hidden
    $procs += [pscustomobject]@{ Slot = $slot; Proc = $p; Log = $log; Killed = $false }
}
Write-Output ("room=" + $Room + " core=" + $Core + " frames=" + $Frames + " players=" + $Players + " ports=" + $Ports + " delay=" + $Delay + " kill=" + $KillSlot)

$start = Get-Date
$killed = $false
while ($true) {
    $alive = @($procs | Where-Object { -not $_.Proc.HasExited })
    if ($alive.Count -eq 0) { break }
    $elapsed = ((Get-Date) - $start).TotalSeconds
    if ($KillSlot -ge 0 -and -not $killed -and $elapsed -ge $KillAfterSec) {
        $victim = $procs | Where-Object { $_.Slot -eq $KillSlot } | Select-Object -First 1
        if ($victim -and -not $victim.Proc.HasExited) {
            $victim.Proc.Kill(); $victim.Killed = $true; $killed = $true
            Write-Output ("killed slot " + $KillSlot + " at " + [int]$elapsed + "s")
        }
    }
    if ($elapsed -gt $TimeoutSec) {
        foreach ($a in $alive) { $a.Proc.Kill(); Write-Output ("HARD TIMEOUT: killed slot " + $a.Slot) }
        break
    }
    Start-Sleep -Milliseconds 500
}
$wall = ((Get-Date) - $start).TotalSeconds

foreach ($x in $procs) {
    $rc = if ($x.Killed) { 'killed' } else { $x.Proc.ExitCode }
    $lines = @()
    if (Test-Path $x.Log) { $lines = Get-Content $x.Log }
    $final = ($lines | Where-Object { $_ -match 'final:' } | Select-Object -Last 1)
    $done = ($lines | Where-Object { $_ -match 'done:' } | Select-Object -Last 1)
    $syncmode = ($lines | Where-Object { $_ -match 'sync hash:' } | Select-Object -First 1)
    $desync = @($lines | Where-Object { $_ -match 'DESYNC' }).Count   # not 'mismatch': the save-size warning contains it
    $timeouts = @($lines | Where-Object { $_ -match 'timed out|connection lost|start barrier failed|session failed' })
    $fin = if ($final) { $final.Substring($final.IndexOf('final:')) } else { 'NONE' }
    Write-Output ("slot " + $x.Slot + " rc=" + $rc + " | " + $fin)
    Write-Output ("   " + $(if ($done) { $done.Substring($done.IndexOf('done:')) } else { 'no done line' }) + " | " + $(if ($syncmode) { $syncmode.Substring($syncmode.IndexOf('sync hash:')) } else { '' }))
    foreach ($rl in @($lines | Where-Object { $_ -match 'rollback-stats|rollback-timing' })) { Write-Output ('   ' + $rl.Substring($rl.IndexOf('rollback-'))) }
    Write-Output ("   desync-lines=" + $desync + " stall-or-timeout-lines=" + $timeouts.Count + $(if ($timeouts.Count) { ' :: ' + ($timeouts[0]) } else { '' }))
}
Write-Output ("wall=" + [int]$wall + "s")
