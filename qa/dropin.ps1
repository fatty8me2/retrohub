# Drop-in QA (headless + muted, LOCAL server only): two retrorun players on a 4-port game (SNES multitap test ROM by default), a third joins the running game
# after -JoinAfterSec into the empty port 2. Checks: all three end on identical output/input hash chains, no DESYNC, the joiner played, the existing players' longest
# frame gap and stalls are reported. Also -Refusals: the clean refusals (host without drop-in port plan, 2P room without the adapter).
# Usage: powershell -File qa\dropin.ps1 [-Server ws://127.0.0.1:8761] [-Build build] [-Relay] [-Frames 10800] [-JoinAfterSec 20] [-Tag dropin]
param(
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Build = 'build',
    [string]$Core = 'snes9x_libretro.dll',
    [string]$Rom = 'deps\testroms\netplay.sfc',
    [string]$System = 'snes',
    [int]$Frames = 10800,            # 180 s of game at 60 fps (headless runs are paced in real time)
    [int]$JoinAfterSec = 20,
    [switch]$Relay,                  # HUBNET_FORCE_RELAY=1 (every packet through the server relay)
    [switch]$Refusals,
    [string]$Tag = ''
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if ($Tag -eq '') { $Tag = if ($Relay) { 'dropin_relay' } else { 'dropin_direct' } }
$exe = Join-Path $root ($Build + '\bin\Release\retrorun.exe')
$corePath = Join-Path $root ('deps\cores\' + $Core)
$romPath = Join-Path $root $Rom
if (-not (Test-Path $romPath)) { python "$PSScriptRoot\make_nesnes_roms.py" (Join-Path $root 'deps\testroms') }
$outDir = Join-Path $root ('qa\out\' + $Tag)
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$env:RETRORUN_MUTE = '1'
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random
$room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'

function Start-Peer([int]$slot, [string[]]$extra) {
    $log = Join-Path $outDir ('slot' + $slot + '.log')
    $sv = Join-Path $outDir ('sv' + $slot); $st = Join-Path $outDir ('st' + $slot)
    New-Item -ItemType Directory -Force -Path $sv, $st | Out-Null
    $args = @('--core', $corePath, '--rom', $romPath, '--system', $System, '--save-dir', $sv, '--state-dir', $st, '--headless', '--frames', $Frames, '--fuzz-input',
        '--net', $Server, '--room', $room, '--slot', $slot, '--rollback', '--delay', 'auto', '--log', $log) + $extra
    $p = Start-Process -FilePath $exe -ArgumentList $args -PassThru -WindowStyle Hidden
    return [pscustomobject]@{ Slot = $slot; Proc = $p; Log = $log }
}
function Wait-Log($peer, [string]$pattern, [int]$sec) {
    $t = Get-Date
    while (((Get-Date) - $t).TotalSeconds -lt $sec) {
        if ((Test-Path $peer.Log) -and (Select-String -Path $peer.Log -Pattern $pattern -Quiet)) { return $true }
        if ($peer.Proc.HasExited) { return (Test-Path $peer.Log) -and (Select-String -Path $peer.Log -Pattern $pattern -Quiet) }
        Start-Sleep -Milliseconds 300
    }
    return $false
}
$fail = 0
if ($Refusals) {
    # a 2P room booted WITHOUT --multiport has no free port to give: the joiner must be refused cleanly (exit 9), the match untouched
    $h = Start-Peer 0 @('--players', '2', '--ports', '0,1,-1,-1'); $g = Start-Peer 1 @('--players', '2', '--ports', '0,1,-1,-1')
    Start-Sleep -Seconds 12
    $j = Start-Peer 2 @('--dropin', '--players', '2', '--ports', '0,1,2,-1', '--name', 'Kai')
    $j.Proc.WaitForExit(40000) | Out-Null
    $rc = if ($j.Proc.HasExited) { $j.Proc.ExitCode } else { 'running' }
    Write-Output "refusal (no multiport): joiner rc=$rc (want 9)"
    if ($rc -ne 9) { $fail++ }
    foreach ($p in @($h, $g, $j)) { if (-not $p.Proc.HasExited) { Stop-Process -Id $p.Proc.Id -Force } }
    if ($fail) { Write-Output 'DROPIN REFUSALS: FAIL'; exit 1 }
    Write-Output 'DROPIN REFUSALS: PASS'; exit 0
}
Write-Output "room=$room relay=$([bool]$Relay) frames=$Frames join after ${JoinAfterSec}s"
$t0 = Get-Date
$h = Start-Peer 0 @('--players', '2', '--ports', '0,1,-1,-1', '--multiport')
$g = Start-Peer 1 @('--players', '2', '--ports', '0,1,-1,-1', '--multiport')
Start-Sleep -Seconds $JoinAfterSec
$j = Start-Peer 2 @('--dropin', '--players', '2', '--ports', '0,1,2,-1', '--name', 'Kai')
$all = @($h, $g, $j)
while (@($all | Where-Object { -not $_.Proc.HasExited }).Count -gt 0) {
    if (((Get-Date) - $t0).TotalSeconds -gt ($Frames / 60 + 240)) { foreach ($p in $all) { if (-not $p.Proc.HasExited) { Stop-Process -Id $p.Proc.Id -Force; Write-Output "HARD TIMEOUT slot $($p.Slot)" } }; $fail++; break }
    Start-Sleep -Seconds 1
}
foreach ($p in $all) {
    $lines = if (Test-Path $p.Log) { Get-Content $p.Log } else { @() }
    $rc = $p.Proc.ExitCode
    $final = $lines | Where-Object { $_ -match 'final:' } | Select-Object -Last 1
    $gap = $lines | Where-Object { $_ -match 'frame-gap: max' } | Select-Object -Last 1
    $stats = $lines | Where-Object { $_ -match 'rollback-stats' } | Select-Object -Last 1
    $desync = @($lines | Where-Object { $_ -match 'DESYNC' }).Count
    Write-Output ("slot $($p.Slot) rc=$rc | " + $(if ($final) { $final.Substring($final.IndexOf('final:')) } else { 'NO FINAL' }))
    if ($gap) { Write-Output ('   ' + $gap.Substring($gap.IndexOf('frame-gap'))) }
    if ($stats) { Write-Output ('   ' + $stats.Substring($stats.IndexOf('rollback-stats'))) }
    foreach ($l in @($lines | Where-Object { $_ -match 'dropin:' } | Select-Object -First 8)) { Write-Output ('   ' + $l.Substring($l.IndexOf('dropin:'))) }
    if ($rc -ne 0 -or -not $final -or $desync) { $fail++; Write-Output "   FAIL slot $($p.Slot): rc=$rc final=$([bool]$final) desync=$desync" }
}
$hs = @($all | ForEach-Object { if (Test-Path $_.Log) { $f = Get-Content $_.Log | Where-Object { $_ -match 'final:' } | Select-Object -Last 1; if ($f) { ($f -replace '.*final: ', '') -replace 'state-hash \w+ ', '' } } })
Write-Output ('final (frame, output, input) per peer: ' + (($hs | Select-Object -Unique) -join ' || '))
if (@($hs | Select-Object -Unique).Count -ne 1 -or $hs.Count -ne 3) { $fail++; Write-Output 'FAIL: final hash chains differ (or a peer has none)' }
$jl = Get-Content $j.Log
$played = $jl | Where-Object { $_ -match 'dropin: playing from frame' } | Select-Object -First 1
if (-not $played) { $fail++; Write-Output 'FAIL: the joiner never started playing' } else { Write-Output ($played.Substring($played.IndexOf('dropin:'))) }
$joined = (Get-Content $h.Log | Where-Object { $_ -match 'joins on port|admitted on port' } | Select-Object -First 1)
if (-not $joined) { $fail++; Write-Output 'FAIL: the host never admitted the joiner' }
Write-Output ("wall=" + [int]((Get-Date) - $t0).TotalSeconds + "s")
if ($fail) { Write-Output "DROPIN QA: FAIL ($fail)"; exit 1 }
Write-Output 'DROPIN QA: PASS'; exit 0
