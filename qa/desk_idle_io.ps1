# The game page's disk work while it sits idle (Library desk pane and Traditional's full-window page), and the idle throttle.
# HEADLESS and muted: a SANDBOX COPY of the build (no userdata), the SDL dummy driver, a scratch vault, USERPROFILE pointed into the sandbox
# (the BIOS search looks in the profile's Desktop folders), nothing launched, no network. Waits while the owner is gaming.
#
# Every run sets HUB_SHOT_DISKIO=1: the hub counts the file-system calls its UI thread makes while the game page draws (qa_diskio.h) and
# writes "diskio" (per frame with calls) and "diskio-sum" (at exit) trace lines. "late" = from 1 s after the page first drew to the end.
# Checks (exit 1 on any failure):
#   desk-*   : the desk pane with a game selected in the list (HUB_SELECT_GAME); late page calls = 0, late workers = 0, and the throttled
#              loop (HUB_IDLE_SIM=bg: the hub pretends to be behind another window) presents at most 12 frames/s in its steady state (idle =
#              about 10). desk-players-focus: the same game opened with HUB_SHOT_DETAILS (the pad focus on Play: its ring pulses, so the
#              frames/s are only reported).
#   playing-*: the same with HUB_SHOT_FAKE_RUNNING (a game "runs"): late page calls = 0 and late workers = 0 (frames/s only reported:
#              the running banner animates).
#   trad-*   : Traditional's open full-window page: late page calls = 0 (its 4 s save-info refresh is reported apart as "periodic"),
#              at most 12 frames/s.
#   race     : the desk list cursor held down for 3 s (about 30 games): page calls while it moves = 0 (the heavy parts of the pane load
#              once the cursor has stopped for 200 ms), the slowest UI frame is reported.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\desk_idle_io.ps1 [-Bin <bin\Release>] [-Out dir] [-Tag name] [-Only regex] [-NoCheck]
#   -NoCheck: report only (the "before" run on an older build).
param([string]$Bin = '', [string]$Out = '', [string]$Tag = 'after', [string]$Only = '', [switch]$NoCheck)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root "build\qa_deskio_out\$Tag" }
$sandbox = Join-Path $root ("build\qa_deskio_" + $Tag)
New-Item -ItemType Directory -Force $Out | Out-Null
Wait-NoGame
& robocopy $Bin $sandbox /MIR /XD userdata roms profile vault /NFL /NDL /NJH /NJS /NP | Out-Null
$exe = Join-Path $sandbox 'RetroHub.exe'
if (-not (Test-Path $exe)) { throw "no RetroHub.exe in $Bin" }

# a ROM folder with some files and a BIOS folder (no BIOS in it), and an empty profile: the GBA link's BIOS search walks them
$roms = Join-Path $sandbox 'roms'
New-Item -ItemType Directory -Force (Join-Path $roms 'BIOS') | Out-Null
for ($i = 0; $i -lt 40; $i++) { $f = Join-Path $roms ("game{0:D2}.gba" -f $i); if (-not (Test-Path $f)) { [IO.File]::WriteAllBytes($f, [byte[]](1..64)) } }
$prof = Join-Path $sandbox 'profile'
New-Item -ItemType Directory -Force (Join-Path $prof 'Desktop') | Out-Null
$romsJson = ($roms -replace '\\', '/')
$quiet = '"show_friend_games": false, "friend_games_v": 1, "setup_done": true, "username_claimed": true, "net_probe_done": true, "desk_card_done": true'
$hubRoms = "{$quiet, ""rom_folders"": [""$romsJson""]}"

$oot = 'switch:0400486024dec000'
$cases = @(
    @{ n = 'desk-players';      layout = 'desk'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_DETAILS = 'fake:n64-online' } },
    @{ n = 'desk-players-focus'; layout = 'desk'; focus = $true; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_DETAILS = 'fake:n64-online' } },
    @{ n = 'desk-players-saves'; layout = 'desk'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_DETAILS = 'fake:n64-online'; HUB_SHOT_TAB = '1' } },
    @{ n = 'desk-gbalink';      layout = 'desk'; v = @{ HUB_SHOT_FAKE_LIB = '8'; HUB_SHOT_GBALINK = 'real'; HUB_SHOT_DETAILS = 'fake:2' }; hub = $hubRoms },
    @{ n = 'desk-packed-gc';    layout = 'desk'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_ISSUE = 'fake:gc-plain:extract'; HUB_SHOT_DETAILS = 'fake:gc-plain' } },
    @{ n = 'desk-packed-3ds';   layout = 'desk'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_ISSUE = 'fake:3ds-plain:extract'; HUB_SHOT_DETAILS = 'fake:3ds-plain' } },
    @{ n = 'desk-wiiu-keys';    layout = 'desk'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_ISSUE = 'fake:wiiu-plain:keys'; HUB_SHOT_DETAILS = 'fake:wiiu-plain' } },
    # (HUB_SHOT_HOOKSHOT opens Ocarina of Time's page itself: the pad focus is on Play, its ring pulses, so frames/s are only reported)
    @{ n = 'desk-oot-about';    layout = 'desk'; focus = $true; v = @{ HUB_SHOT_FAKE_LIB = '1'; HUB_SHOT_HOOKSHOT = 'idle'; HUB_SHOT_DETAILS = $oot } },
    @{ n = 'desk-oot-settings'; layout = 'desk'; focus = $true; v = @{ HUB_SHOT_FAKE_LIB = '1'; HUB_SHOT_HOOKSHOT = 'idle'; HUB_SHOT_DETAILS = $oot; HUB_SHOT_TAB = '3' } },
    @{ n = 'playing-players';   layout = 'desk'; play = $true; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_DETAILS = 'fake:n64-online'; HUB_SHOT_FAKE_RUNNING = 'ext:fake:n64-online' } },
    @{ n = 'playing-gbalink';   layout = 'desk'; play = $true; v = @{ HUB_SHOT_FAKE_LIB = '8'; HUB_SHOT_GBALINK = 'real'; HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_FAKE_RUNNING = 'ext:fake:2' }; hub = $hubRoms },
    @{ n = 'playing-wiiu';      layout = 'desk'; play = $true; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_DETAILS = 'wiiu:0005000010145d00'; HUB_SHOT_FAKE_RUNNING = 'ext:fake:n64-online' } },
    @{ n = 'trad-players';      layout = 'traditional'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_DETAILS = 'fake:n64-online' } },
    @{ n = 'trad-gbalink';      layout = 'traditional'; v = @{ HUB_SHOT_FAKE_LIB = '8'; HUB_SHOT_GBALINK = 'real'; HUB_SHOT_DETAILS = 'fake:2' }; hub = $hubRoms },
    @{ n = 'trad-packed-gc';    layout = 'traditional'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_ISSUE = 'fake:gc-plain:extract'; HUB_SHOT_DETAILS = 'fake:gc-plain' } },
    @{ n = 'trad-wiiu-keys';    layout = 'traditional'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_ISSUE = 'fake:wiiu-plain:keys'; HUB_SHOT_DETAILS = 'fake:wiiu-plain' } },
    @{ n = 'trad-oot-settings'; layout = 'traditional'; v = @{ HUB_SHOT_FAKE_LIB = '1'; HUB_SHOT_HOOKSHOT = 'idle'; HUB_SHOT_DETAILS = $oot; HUB_SHOT_TAB = '3' } },
    @{ n = 'race';              layout = 'desk'; race = $true; v = @{ HUB_SHOT_FAKE_LIB = '60'; HUB_SHOT_FAKE_TITLES = '1'; HUB_SHOT_PAD = 'hold:3:down,wait,wait,wait' } }
)

function Parse-Kv([string]$s) { $h = @{}; foreach ($m in [regex]::Matches($s, '(\w+)=([-\d.]+)')) { $h[$m.Groups[1].Value] = [double]$m.Groups[2].Value }; return $h }

$fail = 0
$report = @()
foreach ($c in $cases) {
    if ($Only -and $c.n -notmatch $Only) { continue }
    Wait-NoGame
    Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
    Remove-Item -Recurse -Force (Join-Path $sandbox 'userdata') -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $sandbox 'userdata') | Out-Null
    $hub = if ($c.hub) { $c.hub } else { "{$quiet}" }
    Set-Content -Encoding ascii (Join-Path $sandbox 'userdata\hub.json') $hub
    $vault = Join-Path $sandbox 'vault'
    Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
    $trace = Join-Path $Out "$($c.n).trace"
    $fps = Join-Path $Out "$($c.n).fps.txt"
    Remove-Item -Force $trace, $fps -ErrorAction SilentlyContinue
    $set = @{ RETROHUB_QA = '1'; SSB64_MUTE = '1'; RETRORUN_MUTE = '1'; N64RUN_MUTE = '1'; RETROHUB_NO_CLOUD = '1'; RETROHUB_NO_UPDATE_CHECK = '1'
              HUB_SCREENSHOT = (Join-Path $Out "$($c.n).bmp"); HUB_SHOT_SIZE = '1920x1080'; HUB_SHOT_SECS = '10'; HUB_SHOT_VAULT = $vault; HUB_SHOT_TRACE = $trace
              HUB_SHOT_LAYOUT = $c.layout; HUB_SHOT_DISKIO = '1'; HUB_FPS_REPORT = $fps; USERPROFILE = $prof }
    $set['HUB_IDLE_SIM'] = if ($c.race) { 'noraster' } else { 'bg,noraster' }
    foreach ($k in $c.v.Keys) { $set[$k] = $c.v[$k] }
    if ($c.layout -eq 'desk' -and -not $c.focus -and $set.ContainsKey('HUB_SHOT_DETAILS')) {   # the game selected in the list (no pad focus ring on Play: it pulses)
        $set['HUB_SELECT_GAME'] = $set['HUB_SHOT_DETAILS']; $set.Remove('HUB_SHOT_DETAILS')
    }
    $set['HUB_SHOT_DISKIO_PATHS'] = '40'
    $saved = @{}
    foreach ($k in $set.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); Set-Item ("env:" + $k) $set[$k] }
    try {
        $p = Start-Process -FilePath $exe -WorkingDirectory $sandbox -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; Write-Host "$($c.n): timed out (ended PID $($p.Id))"; $fail++; continue }
        $p.WaitForExit()
        $code = $p.ExitCode
    } finally {
        foreach ($k in $set.Keys) { if ($null -eq $saved[$k]) { Remove-Item ("env:" + $k) -ErrorAction SilentlyContinue } else { Set-Item ("env:" + $k) $saved[$k] } }
    }
    $lines = if (Test-Path $trace) { Get-Content $trace } else { @() }
    $sum = $lines | Where-Object { $_ -like 'diskio-sum *' } | Select-Object -Last 1
    if (-not $sum) { Write-Host "$($c.n): exit $code, no diskio-sum line (HUB_SHOT_DISKIO not in this build?)"; $fail++; continue }
    $parts = $sum.Substring(11) -split '\| late:'
    $all = Parse-Kv $parts[0]; $late = Parse-Kv $parts[1]
    $fl = if (Test-Path $fps) { (Get-Content $fps -Raw) } else { '' }
    $pps = if ($fl -match 'presented_per_s=([\d.]+)') { [double]$Matches[1] } else { -1 }
    $first = ($lines | Where-Object { $_ -like 'diskio frame=*' } | Select-Object -First 3) -join "`n      "
    $lateLines = @($lines | Where-Object { $_ -like 'diskio frame=* late*' })
    $line = "{0,-19} exit {1}  page frames {2,4}/{3,-4} all: page {4,6} periodic {5,4} workers {6,4} | late ({7:N1} s): page {8,5} periodic {9,3} workers {10,3} ui {11,5} | ui ms max {12:N1} avg {13:N1} | presented/s {14:N1}" -f `
        $c.n, $code, $all.page_frames, $all.frames, $all.page, $all.periodic, $all.workers, ($all.t_end - $all.t0 - $all.warm), $late.page, $late.periodic, $late.workers, $late.ui, $late.ms_max, $late.ms_avg, $pps
    $bad = @()
    if ($code -ne 0) { $bad += "exit $code" }
    if ($all.page_frames -lt 5) { $bad += 'the page was not drawn' }
    if ($c.race) {
        $hold = $lines | Where-Object { $_ -like 'pad-hold *' } | Select-Object -First 1
        $ht = if ($hold -match ' t=([\d.]+)') { [double]$Matches[1] } else { -1 }
        $moves = @($lines | Where-Object { $_ -like 'desk-cursor *' }).Count
        # the cursor moves from the hold's first repeat (0.3 s in) until it stops: the pane settling after the hold ("gamepage-settled ... t=",
        # newer builds) or the end of the hold (older builds, which have no such line)
        $tend = $ht + 3.0
        foreach ($l in ($lines | Where-Object { $_ -like 'gamepage-settled *' })) { if ($l -match ' t=([\d.]+)' -and [double]$Matches[1] -gt $ht + 0.3) { $tend = [math]::Min($tend, [double]$Matches[1] - 0.001); break } }
        $inHold = 0; $holdFrames = 0
        foreach ($l in ($lines | Where-Object { $_ -like 'diskio frame=*' })) {
            if ($l -match ' t=([\d.]+) page=(\d+)' -and $ht -ge 0 -and [double]$Matches[1] -ge $ht + 0.3 -and [double]$Matches[1] -le $tend) { $inHold += [int]$Matches[2]; if ([int]$Matches[2] -gt 0) { $holdFrames++ } }
        }
        $line += " | race: cursor moves $moves, page calls while moving ({0:N2} s) $inHold in $holdFrames frames" -f ($tend - $ht - 0.3)
        if ($moves -lt 15) { $bad += "the cursor moved only $moves times" }
        if (-not $NoCheck -and $inHold -gt 0) { $bad += "page calls while the cursor moves" }
    } else {
        if (-not $NoCheck) {
            if ($late.page -gt 0) { $bad += "late page calls $($late.page)" }
            if ($c.layout -eq 'desk' -and $late.workers -gt 0) { $bad += "late workers $($late.workers)" }
            if ($c.layout -eq 'desk' -and $late.periodic -gt 0) { $bad += "late periodic $($late.periodic)" }
            if (-not $c.play -and -not $c.focus -and $pps -gt 12) { $bad += "presented $pps frames/s (idle throttle not engaged)" }
        }
    }
    if ($bad.Count) { $fail++; $line += "  FAIL: " + ($bad -join '; ') } else { $line += "  ok" }
    Write-Host $line
    if ($lateLines.Count) { Write-Host ("      late: " + (($lateLines | Select-Object -First 3) -join "`n      late: ")) }
    elseif ($first) { Write-Host "      first: $first" }
    $report += $line
}
Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
$report | Set-Content -Encoding utf8 (Join-Path $Out "summary.txt")
Write-Host "summary: $(Join-Path $Out 'summary.txt'); $fail failure(s)"
exit ([int]($fail -gt 0))
