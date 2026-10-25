# Reachability matrix: for every system x action, is the action offered in the UI (or explained when not), and are the launch
# arguments right? Drives RetroHub headless (HUB_SCREENSHOT dummy video driver, no window, no network, muted) with:
#   HUB_SHOT_FAKE_LIB=all   one online catalog game + one uncatalogued game per system, link-cable games, Remix, Kanto Together, Smash Classic
#   HUB_SELECT_GAME=<key> HUB_SHOT_KEYS=i      open that game's details modal; every action prints "act <key> <name> <state>"
#   HUB_SHOT_ACT=<name>                        press that button once (play | online | favorite | savefolder | cheats | controls | desktop)
#   HUB_SHOT_FAKE_ROOM / HUB_SHOT_ROOM_N=2 / HUB_SHOT_ROOM_GAME / HUB_SHOT_HOSTLAUNCH=1 / HUB_SHOT_HOSTCHANGE=other + HUB_SHOT_GUEST_MSG=<file>
#                                              fake room: host picks + launches, guest receives the host's real launch message
# and reads HUB_SHOT_TRACE (act / spawn / external / launch_online / online-route / room-row / room-button / host-msg / notice lines).
# Output: qa\out\reach_matrix.md.   Usage: powershell -File qa\reach_matrix.ps1 [-Exe <RetroHub.exe>] [-Secs 4]
param([string]$Exe = '', [int]$Secs = 4, [int]$Parallel = 4)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Exe) { $Exe = Join-Path $root 'build\bin\Release\RetroHub.exe' }
if (-not (Test-Path $Exe)) { Write-Host "SKIP: $Exe not built"; exit 0 }
$work = Join-Path $env:TEMP ("reach_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory $out -Force | Out-Null

$script:n = 0
function Start-Hub([hashtable]$e) {
    $script:n++
    $trace = Join-Path $work "t$($script:n).trace"
    foreach ($k in (Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' -or $_.Name -like 'RETROHUB_*' }).Name) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    $env:HUB_SCREENSHOT = Join-Path $work "s$($script:n).bmp"
    $env:HUB_SHOT_TRACE = $trace
    $env:HUB_SHOT_FAKE_LIB = 'all'
    $env:HUB_SHOT_SECS = "$Secs"
    $env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
    foreach ($k in $e.Keys) { Set-Item "env:$k" $e[$k] }
    $p = Start-Process $Exe -PassThru -WindowStyle Hidden
    return @{ proc = $p; trace = $trace }
}
function Wait-Hubs($jobs) {
    foreach ($j in $jobs) { if (-not $j.proc.WaitForExit(90000)) { $j.proc.Kill() } }   # only the PIDs we started
}
function Read-Trace($j) { if (Test-Path $j.trace) { return @(Get-Content $j.trace -Encoding UTF8) } else { return @() } }
function Run-Many($specs) {   # specs: array of hashtables (env); returns trace line arrays in order
    $res = @()
    for ($i = 0; $i -lt $specs.Count; $i += $Parallel) {
        $jobs = @()
        foreach ($s in $specs[$i..([Math]::Min($i + $Parallel, $specs.Count) - 1)]) { $jobs += Start-Hub $s }
        Wait-Hubs $jobs
        foreach ($j in $jobs) { $res += , (Read-Trace $j) }
    }
    return , $res
}

# ---- the games in the fake library (keys as built by lib_state.cpp HUB_SHOT_FAKE_LIB=all) ----
# kind: sys short, online catalog game flag, expectations
$games = @(
    @{ key = 'kSmash';           label = 'Smash Classic (native)'; sys = 'native' },
    @{ key = 'fake:remix';       label = 'Smash Remix';            sys = 'n64'; remix = $true },
    @{ key = 'fake:n64-online';  label = 'N64 catalog';            sys = 'n64' },
    @{ key = 'fake:n64-plain';   label = 'N64 uncatalogued';       sys = 'n64' },
    @{ key = 'fake:gb-online';   label = 'GB catalog';             sys = 'gb' },
    @{ key = 'fake:gb-plain';    label = 'GB uncatalogued';        sys = 'gb' },
    @{ key = 'fake:link-pokered'; label = 'GB link cable (Red)';   sys = 'gb'; link = $true },
    @{ key = 'fake:gbc-online';  label = 'GBC catalog';            sys = 'gbc' },
    @{ key = 'fake:gbc-plain';   label = 'GBC uncatalogued';       sys = 'gbc' },
    @{ key = 'fake:gba-online';  label = 'GBA catalog';            sys = 'gba' },
    @{ key = 'fake:gba-plain';   label = 'GBA uncatalogued';       sys = 'gba' },
    @{ key = 'fake:link-pokeemerald'; label = 'GBA link cable (Emerald)'; sys = 'gba'; link = $true },
    @{ key = 'gba:kanto-together'; label = 'Kanto Together';       sys = 'gba'; kanto = $true },
    @{ key = 'fake:nds-online';  label = 'DS catalog';             sys = 'nds' },
    @{ key = 'fake:nds-plain';   label = 'DS uncatalogued';        sys = 'nds' },
    @{ key = 'fake:3ds-online';  label = '3DS catalog';            sys = '3ds' },
    @{ key = 'fake:3ds-plain';   label = '3DS uncatalogued';       sys = '3ds' },
    @{ key = 'fake:gc-online';   label = 'GameCube catalog';       sys = 'gc' },
    @{ key = 'fake:gc-plain';    label = 'GameCube uncatalogued';  sys = 'gc' },
    @{ key = 'fake:wiiu-plain';  label = 'Wii U';                  sys = 'wiiu' },
    @{ key = 'fake:ps1-online';  label = 'PS1 catalog';            sys = 'ps1' },
    @{ key = 'fake:ps1-plain';   label = 'PS1 uncatalogued';       sys = 'ps1' },
    @{ key = 'fake:ps2-plain';   label = 'PS2';                    sys = 'ps2' }
)
$games[0].key = 'native:smash64'   # kNativeKey; verified below against the trace

function Online-Expected($g) {   # supports_online() as designed: catalog-gated for GC / DS / PS1 / Kanto, open for N64 GB GBC GBA 3DS
    if ($g.sys -in 'native', 'n64', 'gb', 'gbc', 'gba', '3ds') { return 'enabled' }
    if ($g.sys -in 'gc', 'nds', 'ps1') { if ($g.key -like '*online*') { return 'enabled' } else { return 'explained' } }
    return 'explained'   # wiiu, ps2: an "Offline only" button with a tooltip, never silence
}
function Cheats-Expected($g) { if ($g.sys -in 'n64', 'gb', 'gbc', 'gba' -and -not $g.kanto) { return 'enabled' } else { return 'hidden' } }

$rows = New-Object System.Collections.Generic.List[object]
function Add-Row($g, $action, $expect, $actual, [bool]$ok) { $rows.Add([pscustomobject]@{ Sys = $g.sys; Game = $g.label; Action = $action; Expected = $expect; Actual = $actual; Ok = $ok }) }
function Actstate($lines, $key, $name) {
    foreach ($l in $lines) { if ($l -like "act $key $name *") { return $l.Substring("act $key $name ".Length) } }
    return $null
}

# ---- pass 1: details modal per game (all actions offered?) + Play offline (ACT=play) ----
$specs = @()
foreach ($g in $games) { $specs += @{ HUB_SELECT_GAME = $g.key; HUB_SHOT_KEYS = 'i'; HUB_SHOT_ACT = 'play' } }
$res = Run-Many $specs
for ($i = 0; $i -lt $games.Count; $i++) {
    $g = $games[$i]; $t = $res[$i]; $k = $g.key
    if (-not ($t | Where-Object { $_ -like "act $k *" })) { Add-Row $g 'details modal' 'opens' 'no act lines (game not in fake lib or modal did not open)' $false; continue }
    $p = Actstate $t $k 'play'
    Add-Row $g 'Play offline (button)' 'enabled' $p ($p -eq 'enabled')
    # launch routing of Play
    $sp = $t | Where-Object { $_ -like 'spawn *' } | Select-Object -First 1
    $ex = $t | Where-Object { $_ -like 'external *' } | Select-Object -First 1
    $nt = ($t | Where-Object { $_ -like 'notice *' }) -join ' / '
    $want = ''; $got = ''; $ok = $false
    switch ($g.sys) {
        'native' { $want = 'BattleShip.exe --from-hub --no-update-check, or a need-ROM toast'; $got = "$sp$nt"; $ok = ($sp -match '\| --from-hub --no-update-check') -or ($nt -match 'ROM|BattleShip') }
        'n64'    { $want = 'n64run.exe --rom --game --save-dir'; $got = "$sp"; $ok = $sp -match 'n64run\.exe \| --rom .* --game .* --save-dir ' }
        { $_ -in 'gb', 'gbc', 'gba', 'nds' } {
            if ($g.kanto) { $want = 'Kanto setup dialog (no spawn)'; $got = 'not driven (installs a package)'; $ok = $true; break }
            $want = "retrorun.exe --core --rom --system $($g.sys) --save-dir --state-dir"; $got = "$sp"
            $ok = ($sp -match 'retrorun\.exe \| --core ') -and ($sp -match " --system $($g.sys) ") -and ($sp -match '--save-dir') -and ($sp -match '--state-dir') -and (($g.sys -eq 'nds') -or ($sp -match '--resume')) }
        'ps1'    { $want = 'retrorun beetle_psx or BIOS prompt'; $got = "$sp$nt"; $ok = ($sp -match 'retrorun\.exe \| --core .*beetle_psx') -or ($nt -match 'BIOS|PlayStation') -or ([bool]$sp -and $sp -match 'retrorun') }
        default  { $want = 'external emulator routed (installed args or install/BIOS dialog)'; $got = "$ex$nt"; $ok = [bool]$ex -or ($nt -match 'BIOS|archive|unpack') }
    }
    if ($g.kanto) { $ok = $true }
    Add-Row $g 'Play offline (launch)' $want $got $ok
    # online button
    $o = Actstate $t $k 'online'; $oe = Online-Expected $g
    if ($g.kanto) { $oe = 'enabled' }
    $oa = if ($o -like 'explained:*') { 'explained' } elseif ($o) { $o } else { 'missing' }
    Add-Row $g 'Play Online / explanation' $oe "$o" ($oa -eq $oe)
    $c = Actstate $t $k 'cheats'; $ce = Cheats-Expected $g
    Add-Row $g 'Cheats button' $ce $c ($c -eq $ce)
    $cn = Actstate $t $k 'controls'
    Add-Row $g 'Controls for this game' 'enabled' $cn ($cn -like 'enabled:*')
    $st = Actstate $t $k 'settings'
    Add-Row $g 'Per-game settings' 'options > 0' $st ($st -like 'options=*')
    $fv = Actstate $t $k 'favorite'
    Add-Row $g 'Favorite' 'enabled' $fv ($fv -eq 'enabled')
    $sf = Actstate $t $k 'savefolder'
    $sfe = if ($g.sys -eq 'native') { 'hidden:native-keeps-own-saves' } else { 'enabled' }
    Add-Row $g 'Save folder' $sfe $sf ($sf -eq $sfe)
    $dk = Actstate $t $k 'desktop'
    Add-Row $g 'Add to desktop' 'enabled' $dk ($dk -eq 'enabled')
}

# ---- pass 2: press each button once on one representative per system (favorite, online, savefolder, cheats, controls, desktop) ----
$reps = $games | Where-Object { $_.key -in 'fake:n64-online', 'fake:gb-online', 'fake:gba-online', 'fake:nds-online', 'fake:3ds-online', 'fake:gc-online', 'fake:wiiu-plain', 'fake:ps1-online', 'fake:ps2-plain', 'fake:gbc-online', 'fake:remix' }
$acts = 'favorite', 'online', 'savefolder', 'cheats', 'controls', 'desktop'
$specs = @(); $meta = @()
foreach ($g in $reps) { foreach ($a in $acts) { $specs += @{ HUB_SELECT_GAME = $g.key; HUB_SHOT_KEYS = 'i'; HUB_SHOT_ACT = $a }; $meta += , @($g, $a) } }
$res = Run-Many $specs
for ($i = 0; $i -lt $meta.Count; $i++) {
    $g = $meta[$i][0]; $a = $meta[$i][1]; $t = $res[$i]; $k = $g.key
    $state = Actstate $t $k $a
    if ($a -eq 'online') {
        $oe = Online-Expected $g
        if ($oe -eq 'enabled') {
            $pr = $t | Where-Object { $_ -like 'play-online *' } | Select-Object -First 1
            $want = if ($g.key -eq 'fake:remix') { 'play-online pending_room_game=remix' } else { 'play-online pending_room_game=<catalog id>' }
            $ok = [bool]$pr -and ($pr -notmatch 'pending_room_game=$')
            if ($g.key -eq 'fake:remix') { $ok = $pr -eq 'play-online pending_room_game=remix' }
            Add-Row $g 'press Play Online' $want "$pr" $ok
        }
        continue
    }
    $tr = $t | Where-Object { $_ -like "$a *" } | Select-Object -First 1
    $want = switch ($a) { 'favorite' { 'favorite <key> flips was/now' } 'savefolder' { 'savefolder <vault dir for key>' } 'cheats' { 'cheats opened <key> (n64/gb/gbc/gba only)' } 'controls' { 'controls tab-for <sys> game=<key>' } 'desktop' { 'desktop <key> (or a notice if not installed)' } }
    $ok = switch ($a) {
        'favorite' { $tr -match "^favorite $([regex]::Escape($k)) was=(\d) now=(\d)$" -and $Matches[1] -ne $Matches[2] }
        'savefolder' { [bool]$tr -and $tr -like '*saves*' }
        'cheats' { if ((Cheats-Expected $g) -eq 'enabled') { $tr -like "cheats opened $k" } else { -not $tr } }
        'controls' { [bool]$tr -and $tr -like "*game=$k" }
        'desktop' { [bool]$tr }
    }
    $got = if ($tr) { $tr } else { '(nothing happened)' }
    Add-Row $g "press $a" $want $got ([bool]$ok)
}

# ---- pass 3: rooms. Host picks the game and presses Launch; a guest then receives the host's own launch message ----
$roomGames = $games | Where-Object { (Online-Expected $_) -eq 'enabled' -and -not ($_.key -like '*plain*' -and $_.sys -in 'nds', 'gc', 'ps1') }
function Room-Id($g) {   # room id = catalog id when known, else the library key (room_id())
    switch ($g.key) {
        'native:smash64' { return 'smash64' } 'fake:remix' { return 'remix' } 'gba:kanto-together' { return 'kanto' }
        'fake:link-pokered' { return 'pokered' } 'fake:link-pokeemerald' { return 'pokeemerald' }
        default { return $null }
    }
}
# catalog ids for the "-online" fakes come from the library trace of the details pass (title) - resolve through a room run per game
# games without a fixed room id are chosen by the room's default (first online game) - so run those with their key as the selected game via catalog lookup below
$hostRes = @()
for ($i = 0; $i -lt $roomGames.Count; $i++) {
    $g = $roomGames[$i]
    $rid = Room-Id $g
    if (-not $rid) {   # resolve the catalog id: the details pass printed the 'online' act with the same key; use lib title via a probe run
        $probe = Run-Many @(@{ HUB_SELECT_GAME = $g.key; HUB_SHOT_KEYS = 'i'; HUB_SHOT_ACT = 'online' })
        $pl = $probe[0] | Where-Object { $_ -like 'play-online *' } | Select-Object -First 1
        $rid = if ($pl) { $pl -replace '^play-online pending_room_game=', '' } else { $g.key }
    }
    $g.rid = $rid
}
$hostSpecs = @(); $guestPrep = @()
foreach ($g in $roomGames) { $hostSpecs += @{ HUB_SHOT_FAKE_ROOM = '1'; HUB_SHOT_ROOM_N = '2'; HUB_SHOT_HOSTLAUNCH = '1'; HUB_SHOT_READY = '1'; HUB_SHOT_ROOM_GAME = $g.rid; HUB_SHOT_SECS = "$($Secs + 2)" } }
$hostRes = Run-Many $hostSpecs
$guestSpecs = @()
for ($i = 0; $i -lt $roomGames.Count; $i++) {
    $g = $roomGames[$i]; $t = $hostRes[$i]
    $msg = ($t | Where-Object { $_ -like 'host-msg *' } | Select-Object -First 1)
    $mf = Join-Path $work "msg$i.json"
    if ($msg) { [IO.File]::WriteAllText($mf, $msg.Substring(9)) } else { [IO.File]::WriteAllText($mf, '{}') }
    $guestSpecs += @{ HUB_SHOT_FAKE_ROOM = '1'; HUB_SHOT_ROOM_N = '2'; HUB_SHOT_HOSTCHANGE = 'other'; HUB_SHOT_ROOM_GAME = $g.rid; HUB_SHOT_GUEST_MSG = $mf; HUB_SHOT_SECS = "$($Secs + 2)" }
}
$guestRes = Run-Many $guestSpecs
for ($i = 0; $i -lt $roomGames.Count; $i++) {
    $g = $roomGames[$i]; $t = $hostRes[$i]; $gt = $guestRes[$i]; $rid = $g.rid
    # host: the game is in the picker, selectable, and Launch is enabled
    $row = $t | Where-Object { $_ -like "room-row host $rid *" } | Select-Object -First 1
    Add-Row $g 'room: host picks game' "room-row host $rid ok" "$row" ([bool]$row -and $row -match ' ok')
    $btn = $t | Where-Object { $_ -like 'room-button host *' } | Select-Object -Last 1
    if ($g.kanto) { Add-Row $g 'room: host Launch (needs one-time Kanto setup)' 'disabled WITH a reason (block=[...])' "$btn" ([bool]$btn -and $btn -match 'block=\[.+\]') }
    else { Add-Row $g 'room: host Launch enabled' 'launch=1 enabled=1' "$btn" ([bool]$btn -and $btn -match 'enabled=1' -and $btn -match 'launch=1') }
    $lo = $t | Where-Object { $_ -like 'launch_online host *' } | Select-Object -First 1
    $sp = $t | Where-Object { $_ -like 'spawn *' } | Select-Object -First 1
    $rt = $t | Where-Object { $_ -like 'online-route *' } | Select-Object -First 1
    $nt = ($t | Where-Object { $_ -like 'notice *' }) -join ' / '
    $want = ''; $ok = $false
    switch ($g.sys) {
        'native' { $want = 'BattleShip.exe + SSB64_LOBBY_HOST/CODE env'; $ok = $sp -match '\| --from-hub --no-update-check' -and $sp -match 'SSB64_LOBBY_HOST=' -and $sp -match 'SSB64_LOBBY_CODE=' }
        'n64'    { $want = 'n64run.exe --net --room --slot 0 --players 2 --ports'; $ok = $sp -match 'n64run\.exe' -and $sp -match '--net ' -and $sp -match '--slot 0' -and $sp -match '--players 2' -and $sp -match '--ports ' }
        { $_ -in 'gb', 'gbc', 'gba' } {
            if ($g.link) { $want = 'retrorun --link --net --players 2 (+ np_link in host msg)'; $ok = $sp -match '--link' -and $sp -match '--net ' -and $sp -match '--players 2' -and ($t -join "`n") -match 'np_link' }
            elseif ($g.kanto) { $want = 'route kanto'; $ok = $rt -match 'kanto' }
            else { $want = "retrorun --system $($g.sys) --net --room --slot 0 --players 2"; $ok = $sp -match 'retrorun\.exe' -and $sp -match " --system $($g.sys) " -and $sp -match '--net ' -and $sp -match '--slot 0' -and $sp -match '--players 2' } }
        'nds'    { $want = 'retrorun --ds-link --net --players 2'; $ok = $sp -match '--ds-link' -and $sp -match '--net ' -and $sp -match '--players 2' }
        'ps1'    { $want = 'retrorun beetle_psx --net --players 2'; $ok = $sp -match 'retrorun\.exe' -and $sp -match '--net ' -and $sp -match '--players 2' }
        'gc'     { $want = 'route dolphin-netplay'; $ok = $rt -match 'dolphin-netplay' }
        '3ds'    { $want = 'route azahar-np3ds'; $ok = $rt -match 'azahar-np3ds' }
        default  { $want = '(none)'; $ok = $false }
    }
    $got = "$sp$rt"; if (-not $got) { $got = "no launch ($lo) $nt" }
    Add-Row $g 'room: host launch args' $want $got ([bool]$ok)
    # guest: sees the host's pick, and launching from the host's message gives matching args
    $grow = $gt | Where-Object { $_ -like "room-row guest $rid *" } | Select-Object -First 1
    Add-Row $g 'room: guest sees host game' "room-row guest $rid ok chosen" "$grow" ([bool]$grow -and $grow -match ' ok')
    $gsp = $gt | Where-Object { $_ -like 'spawn *' } | Select-Object -First 1
    $grt = $gt | Where-Object { $_ -like 'online-route *' -or $_ -like 'guest-route *' } | Select-Object -First 1
    $gwant = ''; $gok = $false
    switch ($g.sys) {
        'native' { $gwant = 'BattleShip.exe + SSB64_LOBBY_JOIN env'; $gok = $gsp -match 'SSB64_LOBBY_JOIN=' }
        'n64'    { $gwant = 'n64run.exe --slot 1 --players 2'; $gok = $gsp -match 'n64run\.exe' -and $gsp -match '--slot 1' -and $gsp -match '--players 2' }
        { $_ -in 'gb', 'gbc', 'gba' } {
            if ($g.link) { $gwant = 'retrorun --link --slot 1 --players 2'; $gok = $gsp -match '--link' -and $gsp -match '--slot 1' }
            elseif ($g.kanto) { $gwant = 'route kanto'; $gok = $grt -match 'kanto' }
            else { $gwant = "retrorun --system $($g.sys) --slot 1 --players 2"; $gok = $gsp -match " --system $($g.sys) " -and $gsp -match '--slot 1' -and $gsp -match '--players 2' } }
        'nds'    { $gwant = 'retrorun --ds-link --slot 1'; $gok = $gsp -match '--ds-link' -and $gsp -match '--slot 1' }
        'ps1'    { $gwant = 'retrorun --net --slot 1'; $gok = $gsp -match '--net ' -and $gsp -match '--slot 1' }
        'gc'     { $gwant = 'route dolphin-netplay-guest'; $gok = $grt -match 'dolphin-netplay' }
        '3ds'    { $gwant = 'route azahar-np3ds'; $gok = $grt -match 'azahar-np3ds' }
    }
    $ggot = "$gsp$grt"; if (-not $ggot) { $ggot = 'no launch: ' + (($gt | Where-Object { $_ -like 'notice *' }) -join ' / ') }
    Add-Row $g 'room: guest join args' $gwant $ggot ([bool]$gok)
}

# link cable must be hidden/explained when 3 players are in the room (max 2)
$lg = $games | Where-Object { $_.key -eq 'fake:link-pokered' }
$r3 = Run-Many @(@{ HUB_SHOT_FAKE_ROOM = '1'; HUB_SHOT_ROOM_N = '3'; HUB_SHOT_ROOM_GAME = 'pokered' })
$row3 = $r3[0] | Where-Object { $_ -like 'room-row host pokered *' } | Select-Object -First 1
Add-Row $lg 'room: 3 players, 2-player link game' 'blocked (too many players) or hidden' "$row3" ((-not $row3) -or $row3 -match 'blocked')

# ---- write the table ----
$fail = @($rows | Where-Object { -not $_.Ok })
$md = New-Object System.Collections.Generic.List[string]
$md.Add('# RetroHub reachability matrix'); $md.Add('')
$md.Add("Generated by qa/reach_matrix.ps1 on $(Get-Date -Format 'yyyy-MM-dd HH:mm'). $($rows.Count) checks, $($fail.Count) failing.")
$md.Add(''); $md.Add('| System | Game | Action | Expected | Actual | Result |'); $md.Add('|---|---|---|---|---|---|')
function Esc($s) { ("$s" -replace '\|', '/' -replace "`r|`n", ' ') }
foreach ($r in $rows) {
    $a = Esc $r.Actual; if ($a.Length -gt 150) { $a = $a.Substring(0, 150) + '...' }
    $md.Add("| $($r.Sys) | $(Esc $r.Game) | $(Esc $r.Action) | $(Esc $r.Expected) | $a | $(if ($r.Ok) { 'PASS' } else { '**FAIL**' }) |")
}
$md.Add(''); $md.Add('## Failures'); $md.Add('')
if ($fail.Count -eq 0) { $md.Add('None.') } else { foreach ($r in $fail) { $md.Add("- $($r.Game) / $($r.Action): expected [$(Esc $r.Expected)], got [$(Esc $r.Actual)]") } }
[IO.File]::WriteAllLines((Join-Path $out 'reach_matrix.md'), $md)
Write-Host "reach_matrix: $($rows.Count) checks, $($fail.Count) failing -> qa\out\reach_matrix.md"
foreach ($r in $fail) { Write-Host "  FAIL $($r.Game) / $($r.Action): $($r.Actual)" }
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($fail.Count -gt 0) { exit 1 }
