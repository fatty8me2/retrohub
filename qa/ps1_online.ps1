# PlayStation (Beetle PSX) online QA matrix: headless retrorun peers on a LOCAL lobby server, OpenBIOS + a license-free homebrew
# (qa\make_ps1_netdisc.py: polls the pads of 4 multitap ports through the real SIO0 protocol, draws everything from the inputs).
# Every scenario must end with: all peers exit 0, no DESYNC, and byte-identical final output/input hashes on every peer.
#
#   powershell -File qa\ps1_online.ps1 -Server ws://127.0.0.1:9207 [-Scenarios all|quick|long|<name,name>] [-Frames 3000]
# Scenarios: lock2 rb2 lock4 rb4 relay_lock2 relay_rb2 relay_rb4 bad_lock2 bad_rb2 bad_rb4 hostsave mismatch long_rb2 long_lock4 long_rb4 kill_rb2
param(
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Scenarios = 'quick',
    [int]$Frames = 3000,
    [int]$LongFrames = 11000,       # ~3 min of game time
    [int]$TimeoutSec = 900,
    [string]$Disc = ''              # default: qa\out\ps1\N.cue (generated)
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$core = Join-Path $root 'build\bin\Release\cores\beetle_psx_libretro.dll'
if (-not (Test-Path $core)) { Write-Host "SKIP: no beetle_psx_libretro.dll (build with MinGW)"; exit 77 }
$outRoot = Join-Path $root 'qa\out\ps1_online'
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
if (-not $Disc) {
    $Disc = Join-Path $root 'qa\out\ps1\N.cue'
    if (-not (Test-Path $Disc)) { python (Join-Path $root 'qa\make_ps1_netdisc.py') (Join-Path $root 'qa\out\ps1') --name N | Out-Null }
}
$env:RETRORUN_MUTE = '1'; $env:RETROHUB_QA = '1'; $env:HUBNET_LOOPBACK = '1'
$tap = @('--opt', 'beetle_psx_enable_multitap_port1=enabled')
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random

# One room: $n peers (slot i owns port i), per-slot extra args / system dir, optional env. Returns a result object.
function Run-Room([string]$Name, [int]$N, [int]$Frames, [string[]]$Extra, [hashtable]$Env = @{}, [hashtable]$PerSlotExtra = @{}, [int]$KillSlot = -1, [int]$KillAfter = 20, [switch]$RandomCards) {
    $out = Join-Path $outRoot $Name
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $saved = @{}
    foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, [string]$Env[$k]) }
    $room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
    $ports = (0..3 | ForEach-Object { if ($_ -lt $N) { "$_" } else { '-1' } }) -join ','
    $procs = @()
    for ($s = 0; $s -lt $N; $s++) {
        $sv = Join-Path $out "sv$s"; $st = Join-Path $out "st$s"; $sys = Join-Path $out "sys$s"
        New-Item -ItemType Directory -Force -Path $sv, $st, $sys | Out-Null
        if ($RandomCards) { $rb = New-Object byte[] 131072; (New-Object System.Random (1000 + $s)).NextBytes($rb); [IO.File]::WriteAllBytes((Join-Path $sv 'N.srm'), $rb) }   # a different memory card per PC
        $a = @('--core', $core, '--rom', $Disc, '--system', 'ps1', '--system-dir', $sys, '--save-dir', $sv, '--state-dir', $st, '--headless',
               '--frames', $Frames, '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $s, '--players', $N, '--ports', $ports,
               '--delay', 'auto', '--log', (Join-Path $out "slot$s.log")) + $Extra
        if ($PerSlotExtra.ContainsKey($s)) { $a += $PerSlotExtra[$s] }
        $procs += [pscustomobject]@{ Slot = $s; P = (Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden); Killed = $false }
    }
    $t0 = Get-Date
    $killed = $false
    while (@($procs | Where-Object { -not $_.P.HasExited }).Count -gt 0) {
        $el = ((Get-Date) - $t0).TotalSeconds
        if ($KillSlot -ge 0 -and -not $killed -and $el -ge $KillAfter) { $v = $procs | Where-Object { $_.Slot -eq $KillSlot }; if (-not $v.P.HasExited) { $v.P.Kill(); $v.Killed = $true; $killed = $true } }
        if ($el -gt $TimeoutSec) { foreach ($x in $procs) { if (-not $x.P.HasExited) { $x.P.Kill() } }; break }
        Start-Sleep -Milliseconds 500
    }
    foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    $res = [pscustomobject]@{ Name = $Name; N = $N; Wall = [int]((Get-Date) - $t0).TotalSeconds; Rc = @(); Hash = @(); Frames = @(); Desync = 0; Notes = @(); Ok = $true; Killed = $KillSlot }
    foreach ($x in $procs) {
        $txt = if (Test-Path (Join-Path $out "slot$($x.Slot).log")) { Get-Content (Join-Path $out "slot$($x.Slot).log") -Raw } else { '' }
        $m = [regex]::Match($txt, 'final: frame (\d+) state-hash ([0-9a-f]+) output-hash ([0-9a-f]+) input-hash ([0-9a-f]+)')
        $res.Rc += $(if ($x.Killed) { 'killed' } else { $x.P.ExitCode })
        $res.Hash += $(if ($m.Success) { $m.Groups[3].Value + '/' + $m.Groups[4].Value } else { 'none' })
        $res.Frames += $(if ($m.Success) { [int]$m.Groups[1].Value } else { 0 })
        $res.Desync += ([regex]::Matches($txt, 'DESYNC')).Count
        $rs = [regex]::Match($txt, 'rollback-stats: input-delay (\d+) frames \(range (\d+)-(\d+).*?rollbacks (\d+).*?max depth (\d+).*?stalls (\d+).*?hashes compared (\d+)')
        if ($rs.Success) { $res.Notes += "s$($x.Slot): delay $($rs.Groups[2].Value)-$($rs.Groups[3].Value) rollbacks $($rs.Groups[4].Value) depth<=$($rs.Groups[5].Value) stalls $($rs.Groups[6].Value) hashes $($rs.Groups[7].Value)" }
        $ls = [regex]::Match($txt, 'netplay stalls: total (\d+) ms over (\d+) stalled samples.*?final delay (\d+)')
        if ($ls.Success) { $res.Notes += "s$($x.Slot): lockstep stalls $($ls.Groups[1].Value) ms, final delay $($ls.Groups[3].Value)" }
        $hs = [regex]::Matches($txt, 'sync check|hashes compared (\d+)')
    }
    return $res
}

function Judge($r, [int]$WantFrames, [switch]$ExpectFail, [switch]$ExpectKill) {
    $alive = @(0..($r.N - 1) | Where-Object { $r.Rc[$_] -ne 'killed' })
    if ($ExpectFail) {
        $r.Ok = (@($alive | Where-Object { $r.Rc[$_] -eq 7 }).Count -eq $alive.Count)
        $r.Notes += 'expected: every peer refuses with exit 7'
    } elseif ($ExpectKill) {
        $r.Ok = (@($alive | Where-Object { $r.Rc[$_] -eq 4 }).Count -eq $alive.Count) -and $r.Desync -eq 0
        $r.Notes += 'expected: survivors leave with exit 4'
    } else {
        $okRc = (@($alive | Where-Object { $r.Rc[$_] -eq 0 }).Count -eq $alive.Count)
        $same = (@($alive | ForEach-Object { $r.Hash[$_] } | Select-Object -Unique).Count -eq 1) -and $r.Hash[$alive[0]] -ne 'none'
        $fr = (@($alive | Where-Object { $r.Frames[$_] -ge $WantFrames }).Count -eq $alive.Count)
        $r.Ok = $okRc -and $same -and $fr -and $r.Desync -eq 0
        if (-not $okRc) { $r.Notes += 'a peer exited nonzero' }
        if (-not $same) { $r.Notes += 'FINAL HASHES DIFFER' }
        if (-not $fr) { $r.Notes += "frames short of $WantFrames" }
    }
    $line = "{0,-14} peers={1} {2,3}s rc=[{3}] frames=[{4}] desync={5} hash={6} => {7}" -f $r.Name, $r.N, $r.Wall, ($r.Rc -join ','), ($r.Frames -join ','), $r.Desync, ($(if ($r.Hash[0]) { $r.Hash[0] } else { '' })), $(if ($r.Ok) { 'PASS' } else { 'FAIL' })
    Write-Host $line
    foreach ($n in $r.Notes) { Write-Host "    $n" }
    return $r.Ok
}

$sets = @{
    quick = @('lock2', 'rb2', 'lock4', 'rb4', 'relay_rb2', 'hostsave', 'mismatch')
    long  = @('long_rb2', 'long_lock4', 'long_rb4')
    all   = @('lock2', 'rb2', 'lock4', 'rb4', 'relay_lock2', 'relay_rb2', 'relay_rb4', 'bad_lock2', 'bad_rb2', 'bad_rb4', 'hostsave', 'mismatch', 'kill_rb2', 'long_rb2', 'long_lock4', 'long_rb4')
}
$want = if ($sets.ContainsKey($Scenarios)) { $sets[$Scenarios] } else { @($Scenarios.Split(',')) }
$bad = @{ HUBNET_SIM_ENABLE = '1'; HUBNET_SIM_LATENCY_MS = '60'; HUBNET_SIM_JITTER_MS = '25'; HUBNET_SIM_LOSS_PCT = '4'; HUBNET_SIM_SEED = '7' }
$failed = 0
foreach ($sc in $want) {
    $ok = $true
    switch ($sc) {
        'lock2'       { $r = Run-Room $sc 2 $Frames @(); $ok = Judge $r $Frames }
        'rb2'         { $r = Run-Room $sc 2 $Frames @('--rollback', '--rb-pred', '6'); $ok = Judge $r $Frames }
        'lock4'       { $r = Run-Room $sc 4 $Frames $tap; $ok = Judge $r $Frames }
        'rb4'         { $r = Run-Room $sc 4 $Frames ($tap + @('--rollback', '--rb-pred', '6')); $ok = Judge $r $Frames }
        'relay_lock2' { $r = Run-Room $sc 2 $Frames @() @{ HUBNET_FORCE_RELAY = '1' }; $ok = Judge $r $Frames }
        'relay_rb2'   { $r = Run-Room $sc 2 $Frames @('--rollback', '--rb-pred', '6') @{ HUBNET_FORCE_RELAY = '1' }; $ok = Judge $r $Frames }
        'relay_rb4'   { $r = Run-Room $sc 4 $Frames ($tap + @('--rollback', '--rb-pred', '6')) @{ HUBNET_FORCE_RELAY = '1' }; $ok = Judge $r $Frames }
        'bad_lock2'   { $r = Run-Room $sc 2 $Frames @() $bad; $ok = Judge $r $Frames }
        'bad_rb2'     { $r = Run-Room $sc 2 $Frames @('--rollback', '--rb-pred', '8') $bad; $ok = Judge $r $Frames }
        'bad_rb4'     { $r = Run-Room $sc 4 $Frames ($tap + @('--rollback', '--rb-pred', '8')) $bad; $ok = Judge $r $Frames }
        'kill_rb2'    { $r = Run-Room $sc 2 20000 @('--rollback', '--rb-pred', '6') @{} @{} 1 25; $ok = Judge $r 0 -ExpectKill }
        'mismatch' {
            # slot 1 has a (fake) BIOS file in its system dir, slot 0 runs OpenBIOS: the room must refuse to start instead of drifting apart later
            $out = Join-Path $outRoot 'mismatch'
            if (Test-Path $out) { Remove-Item -Recurse -Force $out }
            New-Item -ItemType Directory -Force -Path (Join-Path $out 'sys1') | Out-Null
            [IO.File]::WriteAllBytes((Join-Path $out 'sys1\scph5501.bin'), (New-Object byte[] 524288))
            $room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
            $ps = @()
            foreach ($s in 0, 1) {
                New-Item -ItemType Directory -Force -Path (Join-Path $out "sv$s"), (Join-Path $out "st$s"), (Join-Path $out "sys$s") | Out-Null
                $a = @('--core', $core, '--rom', $Disc, '--system', 'ps1', '--system-dir', (Join-Path $out "sys$s"), '--save-dir', (Join-Path $out "sv$s"), '--state-dir', (Join-Path $out "st$s"),
                       '--headless', '--frames', 600, '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $s, '--players', 2, '--ports', '0,1,-1,-1', '--delay', 'auto', '--log', (Join-Path $out "slot$s.log"))
                $ps += Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden
            }
            foreach ($p in $ps) { if (-not $p.WaitForExit(120000)) { $p.Kill() } }
            $rcs = $ps | ForEach-Object { $_.ExitCode }
            $msg = (Get-Content (Join-Path $out 'slot0.log') | Where-Object { $_ -match 'FAIL: slot' } | Select-Object -First 1)
            $ok = (@($rcs | Where-Object { $_ -eq 7 }).Count -eq 2)
            Write-Host ("{0,-14} peers=2 rc=[{1}] => {2}`n    {3}" -f 'mismatch', ($rcs -join ','), $(if ($ok) { 'PASS' } else { 'FAIL' }), $msg)
        }
        'hostsave' {
            # both peers boot from DIFFERENT random 128 KB memory cards; with --share-host-save the host's card is sent to the guest first,
            # so the emulation (which reads the card) stays identical and the final hashes match
            $r = Run-Room 'hostsave' 2 $Frames @('--share-host-save', '--rollback', '--rb-pred', '6') @{} @{} -1 20 -RandomCards
            $g = Get-Content (Join-Path $outRoot 'hostsave\slot1.log') -Raw
            $shared = $g -match 'hostsave: booted from host save'
            $ok = (Judge $r $Frames) -and $shared
            Write-Host ("    guest booted from the host's memory card: $shared")
        }
        'long_rb2'    { $r = Run-Room $sc 2 $LongFrames @('--rollback', '--rb-pred', '6') $bad; $ok = Judge $r $LongFrames }
        'long_lock4'  { $r = Run-Room $sc 4 $LongFrames $tap $bad; $ok = Judge $r $LongFrames }
        'long_rb4'    { $r = Run-Room $sc 4 $LongFrames ($tap + @('--rollback', '--rb-pred', '6')) $bad; $ok = Judge $r $LongFrames }
        default       { Write-Host "unknown scenario $sc"; $ok = $false }
    }
    if (-not $ok) { $failed++ }
}
Write-Host ''
if ($failed) { Write-Host "RESULT: FAIL ($failed scenario(s))"; exit 1 }
Write-Host 'RESULT: PASS'
exit 0
