# Headless spectate QA (Windows PowerShell 5.1): 2 players + 1..2 spectators through a LOCAL worker, all hidden, all muted.
# Spectators must end on the SAME final hashes as the players (they replay the host's confirmed inputs).
#   powershell -File qa\spectate_online.ps1 -Mode lockstep -Frames 3000
#   powershell -File qa\spectate_online.ps1 -Mode rollback -Frames 3000 -LateSec 6        (spectator joins 6 s after the players started)
#   powershell -File qa\spectate_online.ps1 -Mode rollback -Frames 3600 -LateSec 20 -ExpectLive   (live join: snapshot + catch-up)
# -ExpectLive (rollback): the spectator must join from a live snapshot (not frame 0), be live within -MaxCatchupMs of asking, compare
# agreeing hashes with the host, and no player frame may take longer than -MaxGapMs while it joins (players log frame-gap lines).
# The local worker is started here from -WorkerDir (a BattleShip worktree that has the role=watch seats) on -Port with its own registry
# and persist dir; only that PID is stopped afterwards. Use -Server to reuse an already running local worker instead.
param(
    [ValidateSet('lockstep', 'rollback')][string]$Mode = 'lockstep',
    [string]$Core = 'mgba_libretro.dll',
    [string]$Rom = 'deps\testroms\sram.gba',
    [string]$System = 'gba',
    [int]$Frames = 3000,
    [int]$Spectators = 1,
    [int]$LateSec = 0,
    [string]$Server = '',
    [string]$WorkerDir = 'C:\Users\fatty\Desktop\SmashClassicPort\wt\p3-spectate-srv\server\cloudflare',
    [int]$Port = 8793,
    [string]$Tag = 'spec',
    [int]$TimeoutSec = 600,
    [switch]$Relay,
    [switch]$ExpectLive,
    [int]$MaxCatchupMs = 3000,
    [double]$MaxGapMs = 50
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$wr = $null
if ($Server -eq '') {
    $Server = "ws://127.0.0.1:$Port"
    $env:WRANGLER_REGISTRY_PATH = "C:\rhq\reg$Port"
    $persist = "C:\rhq\$Port"
    New-Item -ItemType Directory -Force -Path $env:WRANGLER_REGISTRY_PATH | Out-Null
    $wl = Join-Path $env:TEMP "spec-wrangler-$Port.log"
    $node = (Get-Command node).Source
    $wr = Start-Process -FilePath $node -ArgumentList @('node_modules\wrangler\bin\wrangler.js', 'dev', '--ip', '127.0.0.1', '--port', $Port, '--persist-to', $persist) `
        -WorkingDirectory $WorkerDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $wl -RedirectStandardError "$wl.err"
    $up = $false
    for ($i = 0; $i -lt 90 -and -not $up; $i++) {
        Start-Sleep -Seconds 1
        try { $r = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/" -TimeoutSec 2; $up = $true } catch { if ($_.Exception.Response) { $up = $true } }
    }
    if (-not $up) { if ($wr) { Stop-Process -Id $wr.Id -Force }; throw "local worker did not start (see $wl)" }
}
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' }
$env:RETRORUN_MUTE = '1'

$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$corePath = Join-Path $root ('build\bin\Release\cores\' + $Core)
if (-not (Test-Path $corePath)) { $corePath = Join-Path $root ('deps\cores\' + $Core) }
if (-not (Test-Path $corePath)) { $corePath = Join-Path 'C:\Users\fatty\Desktop\SmashClassicPort\RetroHub\deps\cores' $Core }
$romPath = Join-Path $root $Rom
if (-not (Test-Path $romPath)) { $romPath = Join-Path 'C:\Users\fatty\Desktop\SmashClassicPort\RetroHub' $Rom }
$outDir = Join-Path $root ('qa\out\' + $Tag)
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random
$Room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
$procs = @()
function Launch([string]$name, [string[]]$extra) {
    $sv = Join-Path $outDir "sv$name"; $st = Join-Path $outDir "st$name"; $log = Join-Path $outDir "$name.log"
    New-Item -ItemType Directory -Force -Path $sv, $st | Out-Null
    $a = @('--core', $corePath, '--rom', $romPath, '--system', $System, '--save-dir', $sv, '--state-dir', $st, '--headless', '--frames', $Frames,
        '--net', $Server, '--room', $Room, '--ports', '0,1,-1,-1', '--log', $log) + $extra
    if ($Mode -eq 'rollback') { $a += '--rollback' }
    if ($ExpectLive) { $a += '--realtime' }   # lockstep headless runs uncapped otherwise; a live join needs a real-time match
    $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden
    return [pscustomobject]@{ Name = $name; Proc = $p; Log = $log }
}
try {
    $procs += Launch 'p0' @('--slot', '0', '--players', '2', '--delay', 'auto', '--fuzz-input')
    $procs += Launch 'p1' @('--slot', '1', '--players', '2', '--delay', 'auto', '--fuzz-input')
    if ($LateSec -gt 0) { Start-Sleep -Seconds $LateSec }
    for ($i = 1; $i -le $Spectators; $i++) { $procs += Launch ("s$i") @('--spectate', '--players', '2') }
    Write-Output "room=$Room mode=$Mode frames=$Frames spectators=$Spectators late=${LateSec}s"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (@($procs | Where-Object { -not $_.Proc.HasExited }).Count -gt 0 -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec) { Start-Sleep -Milliseconds 500 }
    foreach ($x in $procs) { if (-not $x.Proc.HasExited) { Write-Output "TIMEOUT: stopping $($x.Name) pid $($x.Proc.Id)"; Stop-Process -Id $x.Proc.Id -Force } }
} finally {
    if ($wr) { try { Stop-Process -Id $wr.Id -Force } catch {} }
}
$fail = 0
$finals = @{}
foreach ($x in $procs) {
    $lines = if (Test-Path $x.Log) { Get-Content $x.Log } else { @() }
    $final = $lines | Where-Object { $_ -match 'final:' } | Select-Object -Last 1
    $rc = $x.Proc.ExitCode
    $ds = @($lines | Where-Object { $_ -match 'DESYNC' }).Count
    $sp = $lines | Where-Object { $_ -match 'spectate' } | Select-Object -Last 2
    $hv = if ($final) { ($final -replace '.*final: frame \d+ ', '') } else { 'NONE' }
    $finals[$x.Name] = $hv
    Write-Output ("{0} rc={1} desync={2} | {3}" -f $x.Name, $rc, $ds, $hv)
    foreach ($l in $sp) { Write-Output ("     " + $l) }
    if ($rc -ne 0 -or $ds -gt 0 -or $hv -eq 'NONE') { $fail++ }
}
if ($ExpectLive) {
    foreach ($x in $procs) {
        $lines = if (Test-Path $x.Log) { Get-Content $x.Log } else { @() }
        if ($x.Name -like 's*') {
            $live = $lines | Where-Object { $_ -match 'spectate: LIVE at frame (\d+), (\d+) frames behind the host, (\d+) ms after joining \(([^,]+),' } | Select-Object -First 1
            if (-not $live) { $fail++; Write-Output "FAIL: $($x.Name) never got live"; continue }
            [void]($live -match 'LIVE at frame (\d+), (\d+) frames behind the host, (\d+) ms after joining \(([^,]+),')
            $ms = [int]$Matches[3]; $how = $Matches[4]; $liveFrame = $Matches[1]
            $done = $lines | Where-Object { $_ -match 'spectate: done' } | Select-Object -Last 1
            $ok = 0; $bad = 0
            if ($done -match 'hashes ok (\d+) different (\d+)') { $ok = [int]$Matches[1]; $bad = [int]$Matches[2] }
            Write-Output ("  {0}: live at frame {1} after {2} ms via {3}; hashes ok {4} different {5}" -f $x.Name, $liveFrame, $ms, $how, $ok, $bad)
            if ($how -ne 'live snapshot') { $fail++; Write-Output "FAIL: $($x.Name) did not use a live snapshot" }
            if ($ms -gt $MaxCatchupMs) { $fail++; Write-Output "FAIL: $($x.Name) catch-up $ms ms > $MaxCatchupMs" }
            if ($ok -lt 1 -or $bad -gt 0) { $fail++; Write-Output "FAIL: $($x.Name) hash comparisons ok=$ok bad=$bad" }
        } else {
            $g = $lines | Where-Object { $_ -match 'frame-gap: max ([\d.]+) ms, (\d+) gaps over 50' } | Select-Object -Last 1
            $gm = if ($g -match 'max ([\d.]+) ms, (\d+) gaps') { [double]$Matches[1] } else { -1 }
            $win = @($lines | Where-Object { $_ -match 'frame-gap: frame \d+, 2 s max ([\d.]+) ms, spectators 0x[1-9a-f]' } | ForEach-Object { [void]($_ -match '2 s max ([\d.]+) ms'); [double]$Matches[1] })
            $wm = if ($win.Count) { ($win | Measure-Object -Maximum).Maximum } else { 0 }
            $sl = $lines | Where-Object { $_ -match 'spectate: (live join for|sending the live|slot \d+ loaded)' }
            foreach ($l in $sl) { Write-Output ("     " + $l) }
            Write-Output ("  {0}: worst frame gap {1:N1} ms over the match, {2:N1} ms while spectators were connected" -f $x.Name, $gm, $wm)
            if ($wm -gt $MaxGapMs) { $fail++; Write-Output "FAIL: $($x.Name) frame gap $wm ms > $MaxGapMs while a spectator was connected" }
        }
    }
}
# lockstep: state-hash and output-hash must be identical everywhere. rollback: players and spectator compare the output hash.
# a lockstep live join skipped the frames before its snapshot, so its rolling output hash differs by design: compare the state there.
$cmp = @($finals.Values | ForEach-Object { if ($Mode -eq 'lockstep' -and $ExpectLive) { $_ -replace ' output-hash \w+', '' -replace ' input-hash \w+', '' } elseif ($Mode -eq 'rollback') { ($_ -replace 'state-hash \w+ ', '') -replace ' input-hash \w+', '' } else { $_ -replace ' input-hash \w+', '' } } | Select-Object -Unique)
if ($cmp.Count -ne 1) { $fail++; Write-Output "FAIL: final hashes differ: $($cmp -join ' || ')" }
if ($fail) { Write-Output "RESULT: FAIL"; exit 1 }
Write-Output "RESULT: PASS (spectators ended on the players' hashes)"
exit 0
