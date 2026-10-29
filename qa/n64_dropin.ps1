# Headless N64 drop-in QA (Windows PowerShell 5.1): two n64run players (real-time paced, drop-in room) + friends who join the running match into the
# empty controller ports, through a LOCAL worker. Hidden and muted; processes are stopped by PID only.
#   powershell -File qa\n64_dropin.ps1 [-Scenario basic|refuse|oldpeer|rejoin|kill|all] [-Relay] [-Rom "...\Mario Kart 64 (U) [!].z64"]
# basic   2 players, a 3rd drops in after -JoinSec (25 s) and plays on; checks: the joiner's input is present on every PC, RDRAM sync hashes agree on all 3 for
#         2+ minutes after the join, no DESYNC/resync, and prints the stall the join cost the existing players.
# refuse  the room is full: a 2nd and 3rd friend drop in (slot cycling), the 4th is refused (exit 9).
# oldpeer a player without the drop-in capability (N64RUN_NO_DROPIN_CAP=1): the joiner is refused (exit 10), the match is untouched.
# rejoin  the joiner leaves cleanly (its --frames ends), its port frees up, a new friend drops into the same port and plays.
# kill    the joiner is killed by PID (no goodbye): the others drop it, play on, and the port frees up for another friend.
param(
    [string]$Scenario = 'basic',
    [string]$Rom = 'C:\Users\fatty\Desktop\Everything\Games\N64\Mario Kart 64 (U) [!].z64',
    [string]$Server = '',
    [string]$WorkerDir = 'C:\Users\fatty\Desktop\SmashClassicPort\BattleShip\server\cloudflare',
    [int]$Port = 8801,
    [string]$EmuDir = 'C:\Users\fatty\Desktop\SmashClassicPort\RetroHub\deps\emu',
    [string]$Exe = '',
    [int]$JoinSec = 25,
    [int]$Frames = 12000,
    [int]$TimeoutSec = 600,
    [string]$JoinerPriority = 'Normal',   # Idle / BelowNormal: the joiner shares this one PC with the players (a real friend has their own CPU)
    [switch]$Relay
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. "$PSScriptRoot\n64run_headless_lib.ps1"
$tmp = Join-Path $env:TEMP ("n64dropin-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $tmp | Out-Null
if ($Exe -eq '') { foreach ($c in 'build-hub\bin\Release\n64run.exe', 'build\bin\Release\n64run.exe') { if (Test-Path (Join-Path $root $c)) { $Exe = Join-Path $root $c; break } } }
$wr = $null
if ($Server -eq '') {
    $Server = "ws://127.0.0.1:$Port"
    $wl = Join-Path $tmp 'wrangler.log'
    $node = (Get-Command node).Source
    $wr = Start-Process -FilePath $node -ArgumentList @('node_modules\wrangler\bin\wrangler.js', 'dev', '--ip', '127.0.0.1', '--port', $Port, '--persist-to', "$tmp\persist") `
        -WorkingDirectory $WorkerDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $wl -RedirectStandardError "$wl.err"
    $up = $false
    for ($i = 0; $i -lt 90 -and -not $up; $i++) {
        Start-Sleep -Seconds 1
        try { $null = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/" -TimeoutSec 2; $up = $true } catch { if ($_.Exception.Response) { $up = $true } }
    }
    if (-not $up) { Stop-Process -Id $wr.Id -Force; throw "local worker did not start (see $wl)" }
}
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' }
$env:RETROHUB_QA = '1'
$global:allFail = 0

function Hashes($lines) { $h = @{}; $lines | Where-Object { $_ -match 'sync check sample (\d+) RDRAM ([0-9a-f]+)' } | ForEach-Object { $h[[int]$Matches[1]] = $Matches[2] }; return $h }
function Common($a, $b) { return @($a.Keys | Where-Object { $b.ContainsKey($_) } | Sort-Object) }
function Mism($a, $b, $keys) { return @($keys | Where-Object { $a[$_] -ne $b[$_] }) }

# one scenario run: returns the per-process results
function New-Room { return ('{0}-1' -f ([guid]::NewGuid().ToString('N').Substring(0, 6).ToUpper() -replace '[01OIL]', 'X')) }
function Start-Player([string]$name, [int]$slot, [string]$room, [int]$frames, [string]$extraEnv = '') {
    if ($extraEnv) { $kv = $extraEnv.Split('='); Set-Item -Path "Env:\$($kv[0])" -Value $kv[1] }
    $a = "--rom `"$Rom`" --emu-dir `"$EmuDir`" --game mk64 --save-dir `"$tmp\save-$name`" --net $Server --room $room --players 2 --ports 0,1,-1,-1 --slot $slot --dropin-room --headless --realtime --frames $frames --test-input random:42"
    $env:N64RUN_NET_LOG = "$tmp\$name.net.txt"
    $p = Start-HeadlessRun $Exe $a "$tmp\$name"
    if ($extraEnv) { Remove-Item "Env:\$($extraEnv.Split('=')[0])" -ErrorAction SilentlyContinue }
    return @{ P = $p; Base = "$tmp\$name"; Name = $name }
}
function Start-Joiner([string]$name, [int]$slot, [string]$room, [int]$frames, [string]$who) {
    # the hub writes the joiner's slot into the first free port of the plan; slot 2 -> port 2
    $ports = @{ 2 = '0,1,2,-1'; 3 = '0,1,-1,3' }[$slot]
    if (-not $ports) { $ports = '0,1,-1,-1' }
    $a = "--rom `"$Rom`" --emu-dir `"$EmuDir`" --game mk64 --save-dir `"$tmp\save-$name`" --net $Server --room $room --dropin --slot $slot --players 2 --ports $ports --delay auto --name $who --dropin-room --headless --realtime --frames $frames --test-input random:42"
    $env:N64RUN_NET_LOG = "$tmp\$name.net.txt"
    $p = Start-HeadlessRun $Exe $a "$tmp\$name"
    if ($JoinerPriority -ne 'Normal') { try { $p.PriorityClass = $JoinerPriority } catch {} }
    return @{ P = $p; Base = "$tmp\$name"; Name = $name }
}
function Show([string]$title, $res, $items) {
    Write-Output "== $title"
    for ($i = 0; $i -lt $items.Count; $i++) {
        Write-Output ("  {0}: exit={1} visible={2} {3}s" -f $items[$i].Name, $res[$i].Exit, $res[$i].MaxVisible, $res[$i].Seconds)
    }
}
function Fail([string]$m) { Write-Output "FAIL $m"; $global:allFail++ }
function Grep($lines, [string]$pat, [int]$n = 8, [string]$prefix = '') { $lines | Where-Object { $_ -match $pat } | Select-Object -First $n | ForEach-Object { "  $prefix$_" } }

function Check-Hashes($res, $idx, [string]$label, [int]$minCommon, [int]$fromSample) {
    # all processes in $idx must agree on every common sync-check hash at/after $fromSample
    $hs = @{}; foreach ($i in $idx) { $hs[$i] = Hashes $res[$i].Err }
    $ok = $true
    $ref = $idx[0]
    foreach ($i in $idx[1..($idx.Count - 1)]) {
        $c = @(Common $hs[$ref] $hs[$i] | Where-Object { $_ -ge $fromSample })
        $bad = @(Mism $hs[$ref] $hs[$i] $c)
        Write-Output ("  hashes {0}: proc{1} vs proc{2}: common={3} (samples {4}..{5}) mismatches={6}" -f $label, $ref, $i, $c.Count, $(if ($c.Count) { $c[0] } else { '-' }), $(if ($c.Count) { $c[-1] } else { '-' }), $bad.Count)
        if ($c.Count -lt $minCommon -or $bad.Count) { $ok = $false }
    }
    if (-not $ok) { Fail "hash agreement ($label)" }
}

function Scenario-Basic {
    $room = New-Room
    $items = @()
    $items += Start-Player 'p0' 0 $room $Frames
    $items += Start-Player 'p1' 1 $room $Frames
    Start-Sleep -Seconds $JoinSec
    $items += Start-Joiner 'j2' 2 $room 100000 'Kai'
    $res = Wait-HeadlessRuns $items $TimeoutSec
    Show "basic (join after ${JoinSec}s$(if ($Relay) { ', relay' }))" $res $items
    $j = $res[2].Err
    Grep $j 'dropin|spectate' 14 'joiner: '
    Grep $res[0].Err 'dropin' 14 'host:   '
    Grep $res[1].Err 'dropin' 8 'peer:   '
    $join = $null
    if (($j -join "`n") -match 'playing as slot 2 on port 2 from sample (\d+)') { $join = [int]$Matches[1] } else { Fail 'the joiner never started playing' }
    if (-not ($res[0].Err -match 'slot 2 is playing')) { Fail 'the host never saw the joiner live' }
    if (-not ($res[1].Err -match 'port 2 belongs to slot 2')) { Fail 'the other player never bound the port' }
    if ($join) {
        Check-Hashes $res @(0, 1, 2) 'players+joiner' 12 ($join + 1)
        # the joiner's input must be present everywhere: "input check port 2 ... N of M samples carry input"
        foreach ($i in 0, 1, 2) {
            $last = $res[$i].Err | Where-Object { $_ -match 'input check port 2 .*: (\d+) of (\d+) samples' } | Select-Object -Last 1
            if (-not $last) { Fail "proc$i has no input check for port 2"; continue }
            $null = $last -match 'input check port 2 .*: (\d+) of (\d+) samples'
            $real = [int]$Matches[1]; $seen = [int]$Matches[2]
            Write-Output ("  input port 2 on proc{0}: {1} of {2} samples carry input" -f $i, $real, $seen)
            if ($seen -lt 1500 -or $real -lt 0.8 * $seen) { Fail "joiner input missing on proc$i" }
        }
    }
    foreach ($i in 0, 1) {
        Grep $res[$i].Err 'dropin: join window|dropin: stall of|waited .* ms for the acks|netplay stalls|live join stall' 14 "p${i} stall: "
        if ($res[$i].Err -match 'DESYNC at|resync: recovery requested|resync [0-9]/[0-9] complete|resync: failed') { Fail "proc$i saw a desync/resync"; Grep $res[$i].Err 'DESYNC|resync' 5 "p${i}: " }
        if ($res[$i].Exit -ne 0) { Fail "player $i exit $($res[$i].Exit)" }
    }
    Grep $j 'netplay stalls' 2 'joiner stall: '
    if ($res[2].Exit -ne 0) { Fail "joiner exit $($res[2].Exit)" }
    if (($res | Where-Object { $_.MaxVisible -gt 0 }).Count) { Fail 'a visible window' }
}

# the same two players with nobody joining: the stall baseline of this machine for comparison with the join window of 'basic'
function Scenario-Control {
    $room = New-Room
    $items = @()
    $items += Start-Player 'p0' 0 $room $Frames
    $items += Start-Player 'p1' 1 $room $Frames
    $res = Wait-HeadlessRuns $items $TimeoutSec
    Show 'control (no joiner)' $res $items
    foreach ($i in 0, 1) {
        $lat = $res[$i].Err | Where-Object { $_ -match 'latency: sample (\d+) .* stalled (\d+) ms' } | ForEach-Object { $null = $_ -match 'latency: sample (\d+) .* stalled (\d+) ms'; "$($Matches[1]):$($Matches[2])" }
        Write-Output ("  p{0} cumulative stalled ms by sample: {1}" -f $i, (($lat | Select-Object -First 8) -join ' '))
        Grep $res[$i].Err 'netplay stalls' 1 "p${i}: "
    }
}

function Scenario-Refuse {
    $room = New-Room
    $items = @()
    $items += Start-Player 'p0' 0 $room $Frames
    $items += Start-Player 'p1' 1 $room $Frames
    Start-Sleep -Seconds $JoinSec
    $items += Start-Joiner 'j2' 2 $room 100000 'Kai'
    Start-Sleep -Seconds 25
    $items += Start-Joiner 'j3' 2 $room 100000 'Mia'     # the plan says slot 2 (taken): the runner moves on to slot 3
    Start-Sleep -Seconds 25
    $items += Start-Joiner 'j4' 2 $room 100000 'Lee'     # no slot, no port left
    Start-Sleep -Seconds 12
    $res = Wait-HeadlessRuns $items $TimeoutSec
    Show 'refuse (full room)' $res $items
    Grep $res[4].Err 'dropin|netplay' 8 'j4: '
    if ($res[4].Exit -ne 9) { Fail "the 4th friend should be refused with exit 9, got $($res[4].Exit)" }
    foreach ($k in 2, 3) { if (-not ($res[$k].Err -match 'playing as slot')) { Fail "joiner $k never played" } }
    if (-not ($res[3].Err -match 'playing as slot 3 on port 3')) { Fail 'the 2nd friend did not end up as slot 3 / port 3' }
    Check-Hashes $res @(0, 1, 2, 3) 'two joiners' 4 0
    foreach ($i in 0, 1) { if ($res[$i].Exit -ne 0) { Fail "player $i exit $($res[$i].Exit)" } }
}

function Scenario-OldPeer {
    $room = New-Room
    $items = @()
    $items += Start-Player 'p0' 0 $room 5400
    $items += Start-Player 'p1' 1 $room 5400 'N64RUN_NO_DROPIN_CAP=1'
    Start-Sleep -Seconds $JoinSec
    $items += Start-Joiner 'j2' 2 $room 100000 'Kai'
    $res = Wait-HeadlessRuns $items $TimeoutSec
    Show 'oldpeer (a player lacks the drop-in capability)' $res $items
    Grep $res[0].Err 'dropin' 6 'host: '
    if ($res[2].Exit -ne 10) { Fail "expected exit 10 (friend must update), got $($res[2].Exit)" }
    foreach ($i in 0, 1) { if ($res[$i].Exit -ne 0) { Fail "player $i exit $($res[$i].Exit)" } }
    Check-Hashes $res @(0, 1) 'players untouched' 5 0
}

function Scenario-Rejoin([bool]$kill) {
    $room = New-Room
    $items = @()
    $items += Start-Player 'p0' 0 $room ($Frames + 6000)
    $items += Start-Player 'p1' 1 $room ($Frames + 6000)
    Start-Sleep -Seconds $JoinSec
    $first = Start-Joiner 'j2' 2 $room $(if ($kill) { 100000 } else { 3000 }) 'Kai'     # 3000 VIs ~ 50 s of play, then a clean goodbye
    $items += $first
    $t = [Diagnostics.Stopwatch]::StartNew()
    if ($kill) { Start-Sleep -Seconds 45; try { Stop-Process -Id $first.P.Id -Force } catch {} }
    else { while (-not $first.P.HasExited -and $t.Elapsed.TotalSeconds -lt 240) { Start-Sleep -Milliseconds 500 } }
    Start-Sleep -Seconds 12   # the others drop it and free the port
    $items += Start-Joiner 'j2b' 2 $room 100000 'Mia'
    $res = Wait-HeadlessRuns $items $TimeoutSec
    Show $(if ($kill) { 'kill (joiner killed, then another friend drops in)' } else { 'rejoin (joiner leaves, another friend drops in)' }) $res $items
    Grep $res[0].Err 'dropin' 16 'host: '
    Grep $res[3].Err 'dropin' 6 'second joiner: '
    if (-not ($res[0].Err -match 'slot 2 left; port 2 is free again')) { Fail 'the host never freed the port' }
    if (-not ($res[1].Err -match 'slot 2 left; port 2 is free again')) { Fail 'the other player never freed the port' }
    if (-not ($res[3].Err -match 'playing as slot 2 on port 2')) { Fail 'the second friend never played' }
    Check-Hashes $res @(0, 1, 3) 'after the rejoin' 8 0
    foreach ($i in 0, 1) { if ($res[$i].Exit -ne 0) { Fail "player $i exit $($res[$i].Exit)" } }
    if (-not $kill -and $res[2].Exit -ne 0) { Fail "first joiner exit $($res[2].Exit)" }
    if (($res | Where-Object { $_.MaxVisible -gt 0 }).Count) { Fail 'a visible window' }
}

try {
    $list = if ($Scenario -eq 'all') { @('basic', 'refuse', 'oldpeer', 'rejoin', 'kill') } else { @($Scenario.Split(',')) }
    foreach ($s in $list) {
        switch ($s) {
            'basic' { Scenario-Basic }
            'control' { Scenario-Control }
            'refuse' { Scenario-Refuse }
            'oldpeer' { Scenario-OldPeer }
            'rejoin' { Scenario-Rejoin $false }
            'kill' { Scenario-Rejoin $true }
            default { throw "unknown scenario $s" }
        }
    }
} finally {
    if ($wr) { try { Stop-Process -Id $wr.Id -Force } catch {} }
}
Write-Output "  logs: $tmp"
if ($global:allFail) { Write-Output "FAILED ($($global:allFail))"; exit 1 }
Write-Output 'PASS'
exit 0
