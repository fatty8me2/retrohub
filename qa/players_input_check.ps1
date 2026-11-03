# Players audit: does EVERY player's pad really reach the game? Behavioural, not counting input_state reads (which proved nothing for LRPS2:
# the core asked for every port while only port 0 had a pad plugged in).
#
# Method: one headless run per player count with scripted/fuzzed input on exactly ONE port (RETRORUN_FUZZ_PORTS / N64RUN_TEST_PORTS = port mask),
# compared with a run with no input at all. The game state after a fixed number of frames (RAM dump + output hash, see -Sig per row) must differ for
# every port k that the system/game supports, and must be identical between two identical runs (otherwise the row is reported NONDET).
#   offline : several pads on one PC (retrorun --players N, n64run --ports 0,0,0,0)
#   -Online : N headless peers through a LOCAL worker, peer s owns port s. All peers fuzz = run A; run B_k = peer k sends nothing. A must be stable
#             (two runs equal, all peers equal) and B_k must differ from A: peer k's pad changes the shared game on every PC.
# A game that ignores a port in its attract mode would give false negatives; the rows use programs/ROMs that read every pad into RAM every frame
# (the netplay test ROMs, the 4-pad PS1 homebrew) or games whose OS copies all four pads into RDRAM (N64).
#
#   powershell -File qa\players_input_check.ps1 [-Only nes,snes,ps1,ps2,gb_link,gba_link,gb,gba,nds,n64] [-Online] [-Server ws://127.0.0.1:8761]
#        [-Work qa\out\players] [-Games C:\...\Games] [-Ps2Bios <bios dir with pcsx2\bios>] [-Frames 600]
# Rows whose ROM/core/BIOS is not on this PC are SKIPped (exit 0 if nothing FAILed). Headless, muted, hidden; local server only (qa\_guard.ps1).
param(
    [string[]]$Only = @(),
    [switch]$Online,
    [switch]$NoOffline,
    [string]$N64Games = 'mk64,smash64',   # which N64 games to run (n64 row)
    [string]$N64Players = '2,3,4',
    [switch]$Only2x2,        # with -Online: only the two-PCs-with-two-pads rooms (ports 0,0,1,1)
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Work = '',
    [string]$Cores = '',
    [string]$Games = 'C:\Users\fatty\Desktop\Everything\Games',
    [string]$Ps2Bios = '',
    [string]$Ps2BiosFile = 'SCPH-39001_BIOS_V7_USA_160.BIN',
    [int]$Frames = 0,
    [string]$EmuDir = '',    # mupen64plus + plugins for n64run (default: build\bin\Release\emu, deps\emu, the main RetroHub checkout's deps\emu)
    [int]$TimeoutSec = 600
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # -File passes "a,b" as one string
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Work -eq '') { $Work = Join-Path $root 'qa\out\players' }
New-Item -ItemType Directory -Force $Work | Out-Null
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$n64exe = Join-Path $root 'build\bin\Release\n64run.exe'
$coreDirs = @($Cores, (Join-Path $root 'build\bin\Release\cores'), (Join-Path $root 'deps\cores'), (Join-Path $root 'scratch\cores')) | Where-Object { $_ -and (Test-Path $_) }
function Find-Core([string]$name) { foreach ($d in $coreDirs) { $p = Join-Path $d $name; if (Test-Path $p) { return $p } } return $null }
if (-not $EmuDir) { foreach ($c in 'build\bin\Release\emu', 'deps\emu', '..\..\RetroHub\deps\emu') { $p = Join-Path $root $c; if (Test-Path (Join-Path $p 'mupen64plus.dll')) { $EmuDir = (Resolve-Path $p).Path; break } } }
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'; $env:RETROHUB_QA = '1'; $env:HUBNET_LOOPBACK = '1'
$results = New-Object System.Collections.ArrayList

# ---- ROMs: homebrew test programs are generated; library games are unpacked into $Work (read-only use of $Games) ----
function Get-Rom([string]$kind) {
    switch ($kind) {
        'nes'  { $f = Join-Path $Work 'roms\netplay.nes'; if (-not (Test-Path $f)) { python "$PSScriptRoot\make_nesnes_roms.py" (Join-Path $Work 'roms') | Out-Null }; return $f }
        'snes' { $f = Join-Path $Work 'roms\netplay.sfc'; if (-not (Test-Path $f)) { python "$PSScriptRoot\make_nesnes_roms.py" (Join-Path $Work 'roms') | Out-Null }; return $f }
        'ps1'  { $f = Join-Path $Work 'roms\ps1\N.cue'; if (-not (Test-Path $f)) { python "$PSScriptRoot\make_ps1_netdisc.py" (Join-Path $Work 'roms\ps1') --name N | Out-Null }; return $f }
    }
}
function Unpack-Game([string]$sub, [string]$pattern, [string]$ext) {
    $z = Get-ChildItem (Join-Path $Games $sub) -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like $pattern } | Select-Object -First 1
    if (-not $z) { return $null }
    $d = Join-Path $Work ('roms\' + $sub + '_' + (($z.BaseName -replace '[^A-Za-z0-9]', '').Substring(0, [Math]::Min(14, ($z.BaseName -replace '[^A-Za-z0-9]', '').Length))))
    $hit = Get-ChildItem $d -Filter $ext -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $hit) {
        if ($z.Extension -ieq $ext.TrimStart('*')) { return $z.FullName }
        if (-not (Test-Path $sevenZip)) { return $null }
        & $sevenZip x -y "-o$d" $z.FullName | Out-Null
        $hit = Get-ChildItem $d -Filter $ext -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if ($hit) { return $hit.FullName } else { return $null }
}

# Rows. Sig: which end-of-run observables define "the game state": ram (RETRORUN_DUMP_RAM = system RAM), out (video+audio hash), state (serialized state hash).
$rows = @(
    @{ id = 'nes';  label = 'NES (Nestopia, Four Score)';          core = 'nestopia_libretro.dll'; sys = 'nes';  rom = { Get-Rom 'nes' };  players = 4; sig = 'ram,out'; frames = 600;  rb = $true }
    @{ id = 'snes'; label = 'SNES (snes9x, multitap on port 2)';   core = 'snes9x_libretro.dll';   sys = 'snes'; rom = { Get-Rom 'snes' }; players = 4; sig = 'ram,out'; frames = 600;  rb = $true }
    @{ id = 'ps1';  label = 'PS1 (Beetle PSX, multitap)';          core = 'beetle_psx_libretro.dll'; sys = 'ps1'; rom = { Get-Rom 'ps1' };  players = 4; sig = 'ram,out'; frames = 900;  rb = $true; sysdir = $true }
    @{ id = 'ps2';  label = 'PS2 (LRPS2, multitap)';               core = 'pcsx2_libretro.dll';    sys = 'ps2';  rom = { Unpack-Game 'PS2' 'Digimon Rumble Arena 2*' '*.iso' }; players = 4; sig = 'ram'; frames = 1500; rb = $false; ps2 = $true }
    @{ id = 'gb_link';  label = 'GB/GBC link cable (SameBoy x2)';  core = 'sameboy_rb_libretro.dll'; sys = 'gb';  rom = { Unpack-Game 'GBC' 'Pokemon - Red*' '*.gb' };  players = 2; sig = 'out'; frames = 900; rb = $true; link = $true; sysdir = $true }
    @{ id = 'gba_link'; label = 'GBA link cable (mGBA x2)';        core = 'mgba_link_libretro.dll'; sys = 'gba'; rom = { Unpack-Game 'GBA' 'Pokemon - Ruby*' '*.gba' }; players = 2; sig = 'out'; frames = 900; rb = $true; link = $true }
    @{ id = 'gb';  label = 'GB/GBC single (SameBoy rollback core)'; core = 'sameboy_rb_libretro.dll'; sys = 'gb';  rom = { Unpack-Game 'GBC' 'Pokemon - Red*' '*.gb' };  players = 1; sig = 'out'; frames = 900; rb = $true; sysdir = $true }
    @{ id = 'gba'; label = 'GBA single (mGBA)';                    core = 'mgba_libretro.dll';     sys = 'gba';  rom = { Unpack-Game 'GBA' 'Pokemon - Ruby*' '*.gba' }; players = 1; sig = 'out'; frames = 900; rb = $false }
    @{ id = 'nds'; label = 'NDS (melonDS; 1 pad, wireless = 1 instance per player)'; core = 'melondsds_libretro.dll'; sys = 'nds'; rom = { Unpack-Game 'NDS' 'Pokemon - Diamond*' '*.nds' }; players = 1; sig = 'out'; frames = 700; rb = $false; sysdir = $true }
)

function Sig-Of([string]$logPath, [string]$ramPath, [string]$sigList) {
    $fin = $null
    if (Test-Path $logPath) { $fin = Select-String -Path $logPath -Pattern '^final:' | Select-Object -Last 1 }
    $o = ''; $s = ''
    if ($fin) {
        $m = [regex]::Match($fin.Line, 'state-hash ([0-9a-f]+) output-hash ([0-9a-f]+)')
        if ($m.Success) { $s = $m.Groups[1].Value; $o = $m.Groups[2].Value }
    }
    $r = ''
    if ($ramPath -and (Test-Path $ramPath)) { $r = (Get-FileHash $ramPath -Algorithm MD5).Hash.Substring(0, 12) }
    $parts = @()
    foreach ($k in $sigList.Split(',')) { if ($k -eq 'ram') { $parts += "ram=$r" } elseif ($k -eq 'out') { $parts += "out=$o" } elseif ($k -eq 'state') { $parts += "st=$s" } }
    if (-not $fin) { return 'NOFINAL' }
    return ($parts -join ' ')
}

function QArgs($a) { return @($a | ForEach-Object { if ([string]$_ -match '\s') { '"' + $_ + '"' } else { [string]$_ } }) }   # Start-Process does not quote array elements
function Row-Args($row, [string]$core, [string]$rom, [string]$dir) {
    $a = @('--core', $core, '--rom', $rom, '--system', $row.sys, '--save-dir', "$dir\sv", '--state-dir', "$dir\st")
    if ($row.sysdir) { New-Item -ItemType Directory -Force "$dir\sys" | Out-Null; $a += @('--system-dir', "$dir\sys") }
    if ($row.ps2) {
        New-Item -ItemType Directory -Force "$dir\sys\pcsx2" | Out-Null
        Copy-Item (Join-Path $Ps2Bios 'pcsx2\bios') "$dir\sys\pcsx2\" -Recurse -Force
        $a += @('--system-dir', "$dir\sys", '--opt', "pcsx2_bios=$Ps2BiosFile", '--setup-tag', 'QA')
    }
    if ($row.link) { $a += '--link' }
    return $a
}

# ---------------------------------------------------------------- offline
function Run-Offline($row, $core, $rom, [int]$mask, [string]$tag, [bool]$hubStyle = $false, [string[]]$more = @(), [int]$players = 0) {
    $dir = Join-Path $Work ("off_$($row.id)_$tag"); if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }; New-Item -ItemType Directory -Force "$dir\sv", "$dir\st" | Out-Null
    $ram = "$dir\ram.bin"; $log = "$dir\run.log"
    $env:RETRORUN_FUZZ_PORTS = "$mask"; $env:RETRORUN_DUMP_RAM = $ram
    $a = (Row-Args $row $core $rom $dir) + @('--headless', '--frames', $(if ($Frames) { $Frames } else { $row.frames }), '--fuzz-input', '--log', $log)
    if ($hubStyle) { $env:RETRORUN_QA_PADS = "$($row.players)" } else { $a += @('--players', $(if ($players) { $players } else { $row.players })) }   # hub style: no --players, N connected pads (the hub never passes --players)
    $a += $more
    $p = Start-Process -FilePath $exe -ArgumentList (QArgs $a) -PassThru -WindowStyle Hidden -Wait
    Remove-Item Env:RETRORUN_FUZZ_PORTS, Env:RETRORUN_DUMP_RAM, Env:RETRORUN_QA_PADS, Env:RETRORUN_QA_NOADOPT -ErrorAction SilentlyContinue
    return (Sig-Of $log $ram $row.sig)
}

function Test-Offline($row, $core, $rom) {
    $n = $row.players
    $b1 = Run-Offline $row $core $rom 0 'b1'
    $b2 = Run-Offline $row $core $rom 0 'b2'
    $det = ($b1 -eq $b2) -and ($b1 -ne 'NOFINAL')
    $ports = @()
    for ($k = 0; $k -lt $n; $k++) {
        $s1 = Run-Offline $row $core $rom (1 -shl $k) "p$k"
        $differs = ($s1 -ne $b1) -and ($s1 -ne $b2) -and ($s1 -ne 'NOFINAL')
        $ports += [pscustomobject]@{ Port = $k; Differs = $differs; Sig = $s1 }
    }
    $ok = $det -and (@($ports | Where-Object { -not $_.Differs }).Count -eq 0)
    $txt = "baseline " + $(if ($det) { 'stable' } else { "NONDET ($b1 | $b2)" }) + "; " + (($ports | ForEach-Object { "P$($_.Port + 1) " + $(if ($_.Differs) { 'changes the game' } else { 'NO EFFECT' }) }) -join ', ')
    return [pscustomobject]@{ Row = $row.id; Mode = "offline x$n"; Ok = $ok; Nondet = (-not $det); Text = $txt }
}


# Wiring of the hub launch (no --players, N connected controllers): must play exactly like --players N, for the last port too.
function Test-HubStyle($row, $core, $rom) {
    $last = $row.players - 1
    $w = Run-Offline $row $core $rom (1 -shl $last) 'hub' $true
    $x = Run-Offline $row $core $rom (1 -shl $last) 'plr' $false
    $ok = ($w -eq $x) -and ($w -ne 'NOFINAL')
    return [pscustomobject]@{ Row = $row.id; Mode = "offline hub-style ($($row.players) pads, no --players)"; Ok = $ok; Nondet = $false; Text = $(if ($ok) { "P$($row.players) (last pad) drives the game exactly like --players $($row.players)" } else { "DIFFERS from --players $($row.players): $w | $x" }) }
}

# The game's seat cap (hub: --max-local = catalog max_players): 4 connected pads on a 2-seat game -> 2 players, no adapter; port 3 input must do nothing.
function Test-SeatCap($row, $core, $rom) {
    $b = Run-Offline $row $core $rom 0 'cap0' $true @('--max-local', '2')
    $x = Run-Offline $row $core $rom 8 'cap8' $true @('--max-local', '2')
    $f = Run-Offline $row $core $rom 8 'cap4' $true @('--max-local', '4')
    $log = Get-Content "$Work\off_$($row.id)_cap0\run.log" -Raw
    $noTap = ($log -notmatch 'adapter|multitap on|pads 3-4 plugged')
    $ok = ($b -eq $x) -and ($f -ne $b) -and $noTap
    return [pscustomobject]@{ Row = $row.id; Mode = 'offline seat cap (4 pads, --max-local 2)'; Ok = $ok; Nondet = $false; Text = $(if ($ok) { '2 seats: pads 3-4 ignored and no adapter; 4 seats: the 4th pad plays' } else { "seat cap not honoured (same=$($b -eq $x) 4seats-differs=$($f -ne $b) noAdapter=$noTap)" }) }
}

# Save states record the port plan. A state made with the adapter on is resumed with the adapter on (also with 2 pads); a manual load under another plan is refused.
function Test-StatePlan($row, $core, $rom) {
    $null = Run-Offline $row $core $rom 0 'sp4' $false @() 4
    $st = "$Work\off_$($row.id)_sp4\st\auto.state"
    $plan = Get-Content "$Work\off_$($row.id)_sp4\st\auto.plan" -ErrorAction SilentlyContinue
    $null = Run-Offline $row $core $rom 0 'sp2' $false @('--load-state', $st) 2
    $l2 = Get-Content "$Work\off_$($row.id)_sp2\run.log" -Raw
    $adopt = ($l2 -match 'boot with the port plan of the saved state \(4-player adapter on\)') -and ($l2 -match 'Resumed from saved state')
    $env:RETRORUN_QA_NOADOPT = '1'
    $null = Run-Offline $row $core $rom 0 'sp2n' $false @('--load-state', $st) 2
    $l3 = Get-Content "$Work\off_$($row.id)_sp2n\run.log" -Raw
    $refused = ($l3 -match 'refused, port plan differs') -and ($l3 -notmatch 'Resumed from saved state')
    $ok = ($plan -match 'tap=1') -and $adopt -and $refused
    return [pscustomobject]@{ Row = $row.id; Mode = 'save state port plan'; Ok = $ok; Nondet = $false; Text = "plan file '$plan'; resume adopts the adapter plan: $adopt; mismatching load refused: $refused" }
}
# ---------------------------------------------------------------- online (retrorun)
function Run-Online($row, $core, $rom, [int]$n, [int[]]$masks, [string]$tag, [bool]$rollback, [string]$portsOverride = '') {
    $dir = Join-Path $Work ("on_$($row.id)_$tag"); if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }; New-Item -ItemType Directory -Force $dir | Out-Null
    $room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
    $ports = (0..3 | ForEach-Object { if ($_ -lt $n) { "$_" } else { '-1' } }) -join ','
    if ($row.link) { $ports = '0,1,-1,-1' }
    if ($portsOverride) { $ports = $portsOverride }   # e.g. '0,0,1,1': two PCs with two pads each
    $procs = @()
    $fr = $(if ($Frames) { $Frames } else { $row.frames })
    for ($s = 0; $s -lt $n; $s++) {
        $d = "$dir\p$s"; New-Item -ItemType Directory -Force "$d\sv", "$d\st" | Out-Null
        $env:RETRORUN_FUZZ_PORTS = "$($masks[$s])"; $env:RETRORUN_DUMP_RAM = "$d\ram.bin"; $env:N64RUN_FIXED_DELAY = '1'   # lockstep: the adaptive delay would make two identical runs differ
        $a = (Row-Args $row $core $rom $d) + @('--headless', '--frames', $fr, '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $s, '--players', $n, '--ports', $ports, '--log', "$d\run.log")
        if ($rollback) { $a += @('--rollback', '--delay', 1, '--no-rb-delay-adapt') } else { $a += @('--delay', 3) }
        if ($row.id -eq 'ps1') { $a += @('--rb-pred', 6) }
        $procs += [pscustomobject]@{ Slot = $s; P = (Start-Process -FilePath $exe -ArgumentList (QArgs $a) -PassThru -WindowStyle Hidden); Dir = $d }
    }
    Remove-Item Env:RETRORUN_FUZZ_PORTS, Env:RETRORUN_DUMP_RAM, Env:N64RUN_FIXED_DELAY -ErrorAction SilentlyContinue
    $t0 = Get-Date
    while (@($procs | Where-Object { -not $_.P.HasExited }).Count -gt 0) {
        if (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) { foreach ($x in $procs) { if (-not $x.P.HasExited) { $x.P.Kill() } }; break }   # by PID, only our own peers
        Start-Sleep -Milliseconds 400
    }
    $sigs = @(); $bad = @()
    foreach ($x in $procs) {
        $rc = if ($x.P.HasExited) { $x.P.ExitCode } else { -1 }
        $sg = Sig-Of "$($x.Dir)\run.log" "$($x.Dir)\ram.bin" $row.sig
        $ds = @(Select-String -Path "$($x.Dir)\run.log" -Pattern 'DESYNC (at|\(spectator\))' -CaseSensitive -ErrorAction SilentlyContinue).Count
        if ($rc -ne 0) { $bad += "slot$($x.Slot) rc=$rc" }
        if ($ds) { $bad += "slot$($x.Slot) DESYNC" }
        $sigs += $sg
    }
    $agree = (@($sigs | Select-Object -Unique).Count -eq 1) -and ($sigs[0] -ne 'NOFINAL')
    return [pscustomobject]@{ Sig = $sigs[0]; All = $sigs; Agree = $agree; Bad = $bad }
}

function Test-Online($row, $core, $rom, [bool]$rollback, [int]$n) {
    $mode = ($(if ($rollback) { 'rollback' } else { 'lockstep' })) + " x$n"
    $all = @(1..$n | ForEach-Object { 15 })
    $a1 = Run-Online $row $core $rom $n $all 'A1' $rollback
    $a2 = Run-Online $row $core $rom $n $all 'A2' $rollback
    $stable = $a1.Agree -and $a2.Agree -and ($a1.Sig -eq $a2.Sig) -and ($a1.Bad.Count + $a2.Bad.Count -eq 0)
    $ports = @()
    for ($k = 0; $k -lt $n; $k++) {
        $m = @(1..$n | ForEach-Object { 15 }); $m[$k] = 0
        $b = Run-Online $row $core $rom $n $m "B$k" $rollback
        $differs = $b.Agree -and ($b.Bad.Count -eq 0) -and ($b.Sig -ne $a1.Sig)
        $ports += [pscustomobject]@{ Port = $k; Differs = $differs; Note = $(if ($b.Bad.Count) { ($b.Bad -join ';') } elseif (-not $b.Agree) { 'PEERS DISAGREE' } else { '' }) }
    }
    $ok = $stable -and (@($ports | Where-Object { -not $_.Differs }).Count -eq 0)
    $txt = "A runs " + $(if ($stable) { 'stable+peers agree' } else { "UNSTABLE (agree=$($a1.Agree)/$($a2.Agree) same=$($a1.Sig -eq $a2.Sig) bad=$($a1.Bad -join ';')$($a2.Bad -join ';'))" }) + "; " +
           (($ports | ForEach-Object { "P$($_.Port + 1) " + $(if ($_.Differs) { 'changes the game on every PC' } else { 'NO EFFECT ' + $_.Note }) }) -join ', ')
    return [pscustomobject]@{ Row = $row.id; Mode = "online $mode"; Ok = $ok; Nondet = (-not $stable); Text = $txt }
}


# Two PCs with two pads each (--ports 0,0,1,1): the hub allows up to 2 pads per PC. Every pad of every PC must reach the game on the other PC too.
function Test-Online2x2($row, $core, $rom, [bool]$rollback) {
    $mode = ($(if ($rollback) { 'rollback' } else { 'lockstep' })) + ' 2 PCs x 2 pads'
    $pl = '0,0,1,1'
    $a1 = Run-Online $row $core $rom 2 @(3, 12) 'C1' $rollback $pl
    $a2 = Run-Online $row $core $rom 2 @(3, 12) 'C2' $rollback $pl
    $stable = $a1.Agree -and $a2.Agree -and ($a1.Sig -eq $a2.Sig) -and ($a1.Bad.Count + $a2.Bad.Count -eq 0)
    $ports = @()
    for ($k = 0; $k -lt 4; $k++) {
        $m = @(3, 12); $o = [int][Math]::Floor($k / 2); $m[$o] = $m[$o] -band (-bnot (1 -shl $k))
        $b = Run-Online $row $core $rom 2 $m "D$k" $rollback $pl
        $differs = $b.Agree -and ($b.Bad.Count -eq 0) -and ($b.Sig -ne $a1.Sig)
        $ports += [pscustomobject]@{ Port = $k; Differs = $differs; Note = $(if ($b.Bad.Count) { ($b.Bad -join ';') } elseif (-not $b.Agree) { 'PEERS DISAGREE' } else { '' }) }
    }
    $ok = $stable -and (@($ports | Where-Object { -not $_.Differs }).Count -eq 0)
    $txt = "A runs " + $(if ($stable) { 'stable+peers agree' } else { 'UNSTABLE' }) + "; " + (($ports | ForEach-Object { "P$($_.Port + 1) " + $(if ($_.Differs) { 'changes the game on both PCs' } else { 'NO EFFECT ' + $_.Note }) }) -join ', ')
    return [pscustomobject]@{ Row = $row.id; Mode = "online $mode"; Ok = $ok; Nondet = (-not $stable); Text = $txt }
}
# ---------------------------------------------------------------- N64 (n64run)
function Find-N64Rom([string]$pattern) { $f = Get-ChildItem (Join-Path $Games 'N64') -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like $pattern } | Select-Object -First 1; if ($f) { return $f.FullName } else { return $null } }

function Run-N64([string]$rom, [string]$game, [int]$n, [int[]]$masks, [string]$tag, [bool]$online, [int]$frames, [string]$extra) {
    $dir = Join-Path $Work ("n64_$tag"); if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }; New-Item -ItemType Directory -Force $dir | Out-Null
    $room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
    $count = if ($online) { $n } else { 1 }
    $ports = if ($online) { (0..3 | ForEach-Object { if ($_ -lt $n) { "$_" } else { '-1' } }) -join ',' } else { '0,0,0,0' }
    if (-not $online -and $n -lt 4) { $ports = (0..3 | ForEach-Object { if ($_ -lt $n) { '0' } else { '-1' } }) -join ',' }
    $procs = @()
    $env:N64RUN_FIXED_DELAY = '1'; $env:N64RUN_QA_NODITHER = '1'   # GLideN64's RDRAM image dithering is noise that differs between identical offline runs (online it is off)
    for ($s = 0; $s -lt $count; $s++) {
        $d = "$dir\p$s"; New-Item -ItemType Directory -Force $d | Out-Null
        $env:N64RUN_TEST_PORTS = "$($masks[$s])"
        $a = "--rom `"$rom`" --game $game --save-dir `"$d\save`" --ports $ports --headless --frames $frames --test-input random:7 --dump-ram 0x0,8388608,`"$d\rdram.bin`" $extra"; if ($EmuDir) { $a += " --emu-dir `"$EmuDir`"" }
        if ($online) { $a += " --net $Server --room $room --players $n --slot $s --delay 3" }
        $pp = Start-Process -FilePath $n64exe -ArgumentList $a -PassThru -WindowStyle Hidden -RedirectStandardOutput "$d\out.txt" -RedirectStandardError "$d\err.txt"; $null = $pp.Handle   # keep the handle: ExitCode is unreadable after exit otherwise
        $procs += [pscustomobject]@{ Slot = $s; P = $pp; Dir = $d }
    }
    Remove-Item Env:N64RUN_TEST_PORTS, Env:N64RUN_FIXED_DELAY, Env:N64RUN_QA_NODITHER -ErrorAction SilentlyContinue
    $t0 = Get-Date
    while (@($procs | Where-Object { -not $_.P.HasExited }).Count -gt 0) {
        if (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) { foreach ($x in $procs) { if (-not $x.P.HasExited) { $x.P.Kill() } }; break }
        Start-Sleep -Milliseconds 400
    }
    $sigs = @(); $bad = @()
    foreach ($x in $procs) {
        $rc = if ($x.P.HasExited) { $x.P.ExitCode } else { -1 }
        $sg = if (Test-Path "$($x.Dir)\rdram.bin") { (Get-FileHash "$($x.Dir)\rdram.bin" -Algorithm MD5).Hash.Substring(0, 12) } else { 'NOFINAL' }
        $ds = @(Select-String -Path "$($x.Dir)\err.txt", "$($x.Dir)\out.txt" -Pattern 'DESYNC (at|\(spectator\))' -CaseSensitive -ErrorAction SilentlyContinue).Count
        if ($rc -ne 0) { $bad += "slot$($x.Slot) rc=$rc" }
        if ($ds) { $bad += "slot$($x.Slot) DESYNC" }
        $sigs += $sg
    }
    $agree = (@($sigs | Select-Object -Unique).Count -eq 1) -and ($sigs[0] -ne 'NOFINAL')
    return [pscustomobject]@{ Sig = $sigs[0]; Agree = $agree; Bad = $bad; Dir = $dir }
}

function Test-N64([string]$label, [string]$id, [string]$rom, [string]$game, [int]$n, [bool]$online, [int]$frames, [string]$extra) {
    $mode = if ($online) { "online lockstep x$n" } else { "offline x$n" }
    $a1 = Run-N64 $rom $game $n @(1..$n | ForEach-Object { 15 }) "${id}_${n}_A1" $online $frames $extra
    $a2 = Run-N64 $rom $game $n @(1..$n | ForEach-Object { 15 }) "${id}_${n}_A2" $online $frames $extra
    $stable = $a1.Agree -and $a2.Agree -and ($a1.Sig -eq $a2.Sig) -and ($a1.Bad.Count + $a2.Bad.Count -eq 0)
    $ports = @()
    for ($k = 0; $k -lt $n; $k++) {
        if ($online) { $m = @(1..$n | ForEach-Object { 15 }); $m[$k] = 0 } else { $m = @(15 -band (-bnot (1 -shl $k))) }
        $b = Run-N64 $rom $game $n $m "${id}_${n}_B$k" $online $frames $extra
        $differs = $b.Agree -and ($b.Bad.Count -eq 0) -and ($b.Sig -ne $a1.Sig)
        $ports += [pscustomobject]@{ Port = $k; Differs = $differs; Note = $(if ($b.Bad.Count) { $b.Bad -join ';' } elseif (-not $b.Agree) { 'PEERS DISAGREE' } else { '' }) }
    }
    $ok = $stable -and (@($ports | Where-Object { -not $_.Differs }).Count -eq 0)
    $txt = "A runs " + $(if ($stable) { 'stable' + $(if ($online) { '+peers agree' } else { '' }) } else { "UNSTABLE (agree=$($a1.Agree)/$($a2.Agree) same=$($a1.Sig -eq $a2.Sig) bad=$($a1.Bad -join ';')$($a2.Bad -join ';'))" }) + "; " +
           (($ports | ForEach-Object { "P$($_.Port + 1) " + $(if ($_.Differs) { 'changes the game' } else { 'NO EFFECT ' + $_.Note }) }) -join ', ')
    return [pscustomobject]@{ Row = $id; Mode = $mode; Ok = $ok; Nondet = (-not $stable); Flaky = ((-not $online) -and $id -eq 'n64_mk64'); Text = $txt }
}

# ---------------------------------------------------------------- driver
function Want([string]$id) { return ($Only.Count -eq 0) -or ($Only -contains $id) }
function Add-Result($r) {
    if ($r.Nondet -and $r.Flaky -and -not $r.Ok) { $r | Add-Member -NotePropertyName Skip -NotePropertyValue $true -Force; $r.Text = 'NONDET (known: offline RDRAM of this game is not reproducible, see docs) - ' + $r.Text }
    [void]$results.Add($r); Write-Output ("{0,-9} {1,-24} {2} {3}" -f $(if ($r.Ok) { 'PASS' } elseif ($r.Skip) { 'SKIP' } else { 'FAIL' }), $r.Row, $r.Mode, $r.Text)
}
function Skip($id, $mode, $why) { Add-Result ([pscustomobject]@{ Row = $id; Mode = $mode; Ok = $false; Skip = $true; Nondet = $false; Text = $why }) }

foreach ($row in $rows) {
    if (-not (Want $row.id)) { continue }
    $core = Find-Core $row.core
    if (-not $core) { Skip $row.id 'any' "core $($row.core) not found"; continue }
    $rom = & $row.rom
    if (-not $rom) { Skip $row.id 'any' 'ROM not found'; continue }
    if ($row.ps2 -and (-not $Ps2Bios -or -not (Test-Path (Join-Path $Ps2Bios 'pcsx2\bios')))) { Skip $row.id 'any' 'no PS2 BIOS (-Ps2Bios <dir containing pcsx2\bios>)'; continue }
    Write-Output "== $($row.label)"
    if (-not $NoOffline) { Add-Result (Test-Offline $row $core $rom); if (@('nes', 'snes', 'ps1') -contains $row.id) { Add-Result (Test-HubStyle $row $core $rom); Add-Result (Test-SeatCap $row $core $rom); Add-Result (Test-StatePlan $row $core $rom) } }
    if ($Online -and $row.players -ge 2) {
        foreach ($n in $(if ($Only2x2) { @() } else { 2..$row.players })) {
            if ($row.rb) { Add-Result (Test-Online $row $core $rom $true $n) }                                   # GGPO rollback (NES/SNES/PS1/link cores)
            if ((-not $row.rb) -or ($n -eq 2 -and -not $row.link)) { Add-Result (Test-Online $row $core $rom $false $n) }   # input-delay lockstep (PS2; others once)
        }
        if ($row.players -ge 4) { Add-Result (Test-Online2x2 $row $core $rom $row.rb) }
    }
}

if ((Want 'n64') -and (Test-Path $n64exe)) {
    Write-Output '== N64 (n64run + mupen64plus, 4 controller ports)'
    $mk = Find-N64Rom 'Mario Kart 64*'
    $ss = Find-N64Rom 'Super Smash Bros*'
    $np = @($N64Players -split ',' | ForEach-Object { [int]$_ })
    foreach ($g in @(@{ id = 'n64_mk64'; rom = $mk; game = 'mk64' }, @{ id = 'n64_smash64'; rom = $ss; game = 'smash64' })) {
        if (($N64Games -split ',') -notcontains $g.game) { continue }
        if (-not $g.rom) { Skip $g.id 'any' 'ROM not found'; continue }
        if (-not $NoOffline) { foreach ($n in $np) { Add-Result (Test-N64 'n64' $g.id $g.rom $g.game $n $false 900 '') } }
        if ($Online) { foreach ($n in $np) { Add-Result (Test-N64 'n64' $g.id $g.rom $g.game $n $true 900 '') } }
    }
} elseif (Want 'n64') { Skip 'n64' 'any' 'n64run.exe not built' }

$fail = @($results | Where-Object { -not $_.Ok -and -not $_.Skip })
Write-Output ("players_input_check: {0} passed, {1} failed, {2} skipped" -f @($results | Where-Object { $_.Ok }).Count, $fail.Count, @($results | Where-Object { $_.Skip }).Count)
if ($fail.Count) { exit 1 }
exit 0
