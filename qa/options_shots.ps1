# Headless screenshots of the game option pages drawn with the UI kit (settings_games_ui.cpp): Settings > Video & Games > Game defaults for
# several consoles (scrolled so the block is in view), a game's Settings tab for games of several consoles, Ocarina of Time on Switch
# ("Plays with", the Fritchii rows), at 1280x720 / 1920x1080, one 1024x600, one UI size 150 %, the Light theme, and pad walks (close a
# section, open a dropdown, pick a value, go on past the bottom). After the pad walks it prints the game_settings.json the walk wrote and
# the "gs-set" trace lines, so the key / value written can be checked.
# Runs a SANDBOX COPY of build\bin\Release (no userdata) with a scratch vault, muted, SDL dummy driver: no window is ever shown.
# Refuses to start while a game runs. Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\options_shots.ps1 [-Bin dir] [-Out dir] [-Only regex]
param([string]$Bin = '', [string]$Out = '', [string]$Only = '.', [string]$W720 = '760,420,18', [string]$W1080 = '1100,620,14', [string]$W600 = '620,340,19')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'build\qa_options_out' }
$src = if ($Bin) { $Bin } else { Join-Path $root 'build\bin\Release' }
$box = Join-Path $root 'build\qa_options_box'
$busy = Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
    Where-Object { -not $_.Path -or (($_.Path -notlike "$(Split-Path $root)\*") -and ($_.Path -notlike '*\Temp\*')) }
if ($busy) { Write-Output "A game is running ($($busy.Name -join ', ')): not starting."; exit 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
if (Test-Path $box) { Remove-Item -Recurse -Force $box }
New-Item -ItemType Directory -Force $box | Out-Null
Get-ChildItem $src | Where-Object { $_.Name -ne 'userdata' } | ForEach-Object { Copy-Item -Recurse -Force $_.FullName $box }
Add-Type -AssemblyName System.Drawing

# Settings > Video & Games is a long page: these wheel notches bring the Game defaults block into view (x,y = a point on the page).
$w720 = "HUB_SHOT_WHEEL=$W720"
$w1080 = "HUB_SHOT_WHEEL=$W1080"
$w600 = "HUB_SHOT_WHEEL=$W600"
$setv = 'HUB_SHOT_PAGE=settings:video;HUB_SHOT_FAKE_LIB=6'
$oot = 'HUB_SHOT_FAKE_LIB=1;HUB_SHOT_TAB=3'
$d8 = (@('down') * 8) -join ','
$d16 = (@('down') * 16) -join ','
$runs = @(
    # Settings > Video & Games > Game defaults
    @{ n = 'defs_n64_720';      size = '1280x720';  env = "$setv;HUB_SHOT_GS_SYS=n64;$w720" },
    @{ n = 'defs_ps1_720';      size = '1280x720';  env = "$setv;HUB_SHOT_GS_SYS=ps1;$w720" },
    @{ n = 'defs_switch_720';   size = '1280x720';  env = "$setv;HUB_SHOT_GS_SYS=switch;$w720" },
    @{ n = 'defs_wiiu_720';     size = '1280x720';  env = "$setv;HUB_SHOT_GS_SYS=wiiu;$w720" },
    @{ n = 'defs_nes_720';      size = '1280x720';  env = "$setv;HUB_SHOT_GS_SYS=nes;$w720" },
    @{ n = 'defs_switch2_720';  size = '1280x720';  env = "$setv;HUB_SHOT_GS_SYS=switch2;$w720" },
    @{ n = 'defs_n64_1080';     size = '1920x1080'; env = "$setv;HUB_SHOT_GS_SYS=n64;$w1080" },
    @{ n = 'defs_ps1_1080';     size = '1920x1080'; env = "$setv;HUB_SHOT_GS_SYS=ps1;$w1080" },
    @{ n = 'defs_switch_1080';  size = '1920x1080'; env = "$setv;HUB_SHOT_GS_SYS=switch;$w1080" },
    @{ n = 'defs_wiiu_1080';    size = '1920x1080'; env = "$setv;HUB_SHOT_GS_SYS=wiiu;$w1080" },
    @{ n = 'defs_nes_1080';     size = '1920x1080'; env = "$setv;HUB_SHOT_GS_SYS=nes;$w1080" },
    @{ n = 'defs_n64_600';      size = '1024x600';  env = "$setv;HUB_SHOT_GS_SYS=n64;$w600" },
    @{ n = 'defs_switch_720_s150'; size = '1280x720'; env = "$setv;HUB_SHOT_GS_SYS=switch;HUB_UI_SCALE=150;HUB_SHOT_WHEEL=760,420,21" },
    @{ n = 'defs_n64_720_light';   size = '1280x720'; env = "$setv;HUB_SHOT_GS_SYS=n64;HUB_THEME=light;$w720" },
    @{ n = 'defs_switch_1080_light'; size = '1920x1080'; env = "$setv;HUB_SHOT_GS_SYS=switch;HUB_THEME=light;$w1080" },
    # a game's Settings tab
    @{ n = 'game_n64_720';      size = '1280x720';  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3' },
    @{ n = 'game_ps1_720';      size = '1280x720';  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:ps1-plain;HUB_SHOT_TAB=3' },
    @{ n = 'game_snes_720';     size = '1280x720';  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:snes-online;HUB_SHOT_TAB=3' },
    @{ n = 'game_wiiu_720';     size = '1280x720';  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:wiiu-plain;HUB_SHOT_TAB=3' },
    @{ n = 'game_nds_1080';     size = '1920x1080'; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:nds-plain;HUB_SHOT_TAB=3' },
    @{ n = 'game_n64_1080';     size = '1920x1080'; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3' },
    @{ n = 'game_n64_600';      size = '1024x600';  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3' },
    @{ n = 'game_n64_720_s150'; size = '1280x720';  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3;HUB_UI_SCALE=150' },
    @{ n = 'game_ps1_720_light'; size = '1280x720'; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:ps1-plain;HUB_SHOT_TAB=3;HUB_THEME=light' },
    # Ocarina of Time (Switch 2): Plays with + the Fritchii rows
    @{ n = 'oot_idle_720';      size = '1280x720';  env = "$oot;HUB_SHOT_HOOKSHOT=idle" },
    @{ n = 'oot_fritchii_720';  size = '1280x720';  env = "$oot;HUB_SHOT_HOOKSHOT=done_fritchii" },
    @{ n = 'oot_fritchii_1080'; size = '1920x1080'; env = "$oot;HUB_SHOT_HOOKSHOT=done_fritchii" },
    @{ n = 'oot_fritchii_600';  size = '1024x600';  env = "$oot;HUB_SHOT_HOOKSHOT=done_fritchii" },
    @{ n = 'oot_fritchii_light'; size = '1280x720'; env = "$oot;HUB_SHOT_HOOKSHOT=done_fritchii;HUB_THEME=light" },
    # the Switch-wide "play with" row (Settings > Video & Games, above Game defaults)
    @{ n = 'nxwide_720';        size = '1280x720';  env = "$setv;HUB_SHOT_WHEEL=760,420,12" },
    @{ n = 'nxwide_1080';       size = '1920x1080'; env = "$setv;HUB_SHOT_WHEEL=1100,620,11" },
    # changed rows and pills (seeded game_settings.json), closed sections with their summary (seeded hub.json "option_sections")
    @{ n = 'seed_game_n64_720'; size = '1280x720';  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3';
       seed = '{"version":"1","systems":{},"games":{"fake:n64-plain":{"n64.aa":"4","n64.fb_emulation":"off","_t":"1"}}}'; hub = 'Graphics=0,Advanced=1' },
    @{ n = 'seed_game_n64_1080'; size = '1920x1080'; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3';
       seed = '{"version":"1","systems":{},"games":{"fake:n64-plain":{"n64.aa":"4","n64.fb_emulation":"off","_t":"1"}}}'; hub = 'Graphics=0,Advanced=1' },
    @{ n = 'seed_game_snes_720'; size = '1280x720'; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:snes-online;HUB_SHOT_TAB=3;HUB_SHOT_WHEEL=640,480,3';
       seed = '{"version":"1","systems":{},"games":{"fake:snes-online":{"snes.region":"pal","_t":"1"}}}'; hub = 'Graphics=0' },
    @{ n = 'seed_defs_switch2_720'; size = '1280x720'; env = "$setv;HUB_SHOT_GS_SYS=switch2;$w720";
       seed = '{"version":"1","systems":{"switch":{"switch.vsync":"mailbox","_t":"1"},"switch2":{"switch.res":"6","_t":"1"}},"games":{}}' },
    @{ n = 'seed_defs_n64_720'; size = '1280x720';  env = "$setv;HUB_SHOT_GS_SYS=n64;$w720";
       seed = '{"version":"1","systems":{"n64":{"n64.res":"2","_t":"1"}},"games":{}}'; hub = 'Graphics=0,Audio=0' },
    @{ n = 'seed_defs_n64_light'; size = '1280x720'; env = "$setv;HUB_SHOT_GS_SYS=n64;HUB_THEME=light;$w720";
       seed = '{"version":"1","systems":{"n64":{"n64.res":"2","n64.fb_emulation":"on","_t":"1"}},"games":{}}'; hub = 'Graphics=0,Advanced=1' },
    # pad walks (HUB_SHOT_TRACE lines in <name>.trace.txt; the script prints the gs-* lines and the game_settings.json written)
    #  game page: close Graphics, Volume one step down, open Advanced, open a dropdown, move, pick, put it back with X, go on past the bottom
    @{ n = 'pad_game';          size = '1280x720';  secs = 18; env = "HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3;HUB_SHOT_PAD=down,down,shot:hdr,a,shot:gfx_closed,down,down,left,shot:vol,down,a,shot:adv_open,down,a,shot:dd_open,down,shot:dd_moved,a,shot:dd_picked,x,shot:reset,$d8,shot:bottom" },
    #  Settings page: the System dropdown opened, B closes only the list (still on Settings), then sections and on past the bottom
    @{ n = 'pad_defs';          size = '1280x720';  secs = 20; env = "$setv;HUB_SHOT_GS_SYS=n64;HUB_SHOT_PAD=$d8,down,down,down,down,a,shot:sys_open,down,shot:sys_moved,b,shot:sys_b,down,a,shot:gfx_closed,down,down,down,a,shot:adv_open,$d8,shot:bottom" },
    #  Ocarina of Time: down to "Plays with", pick another program, on past the bottom
    @{ n = 'pad_oot';           size = '1280x720';  secs = 20; env = "$oot;HUB_SHOT_HOOKSHOT=done_fritchii;HUB_SHOT_PAD=$d16,down,down,down,shot:runner,a,shot:runner_open,down,a,shot:runner_picked,$d8,shot:bottom" },
    #  Reset to defaults: every section closed, down to the button, the confirm dialog, Reset
    @{ n = 'pad_reset';         size = '1280x720';  secs = 12; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3;HUB_SHOT_PAD=down,down,down,down,down,shot:button,a,shot:dialog,left,a,shot:done';
       seed = '{"version":"1","systems":{},"games":{"fake:n64-plain":{"n64.aa":"4","n64.fb_emulation":"off","_t":"1"}}}'; hub = 'Graphics=0,Audio=0,Advanced=0' },
    #  open / closed is remembered across restarts and by heading, not by game: close Graphics on one game, restart, look at another
    @{ n = 'pad_persist_a';     size = '1280x720';  secs = 8;  env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:n64-plain;HUB_SHOT_TAB=3;HUB_SHOT_PAD=down,down,a,shot:closed' },
    @{ n = 'pad_persist_b';     size = '1280x720';  keep = $true; env = 'HUB_SHOT_FAKE_LIB=all;HUB_SHOT_DETAILS=fake:ps1-plain;HUB_SHOT_TAB=3' }
)
$fail = 0
foreach ($r in $runs) {
    if ($r.n -notmatch $Only) { continue }
    $vault = Join-Path $box 'vault'
    if (-not $r.keep) {   # (keep = the previous run's userdata and vault: a restart)
        if (Test-Path "$box\userdata") { Remove-Item -Recurse -Force "$box\userdata" }
        if (Test-Path $vault) { Remove-Item -Recurse -Force $vault }
    }
    New-Item -ItemType Directory -Force $vault | Out-Null
    if ($r.seed) {
        New-Item -ItemType Directory -Force (Join-Path $vault '_settings') | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $vault '_settings\game_settings.json'), $r.seed)
    }
    if ($r.hub) {
        New-Item -ItemType Directory -Force "$box\userdata" | Out-Null
        [System.IO.File]::WriteAllText("$box\userdata\hub.json", "{`"setup_done`": true, `"find_games_done`": true, `"option_sections`": `"$($r.hub)`"}")
    }
    $bmp = Join-Path $Out "$($r.n).bmp"
    $trace = Join-Path $Out "$($r.n).trace.txt"
    if (Test-Path $trace) { Remove-Item -Force $trace }
    Get-ChildItem $Out -Filter "$($r.n)*.png" | Where-Object { $_.Name -match "^$([regex]::Escape($r.n))(-.*)?\.png$" } | Remove-Item -Force
    $secs = if ($r.secs) { $r.secs } else { 5 }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $box 'RetroHub.exe'
    $psi.WorkingDirectory = $box
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($kv in @("HUB_SCREENSHOT=$bmp", "HUB_SHOT_SIZE=$($r.size)", "HUB_SHOT_SECS=$secs", "HUB_SHOT_VAULT=$vault", "HUB_SHOT_TRACE=$trace",
                      'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETROHUB_NO_CLOUD=1') + ($r.env -split ';')) {
        if ($kv -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; Write-Output "$($r.n): TIMEOUT"; $fail++; continue }
    $made = Get-ChildItem $Out -Filter "$($r.n)*.bmp"
    foreach ($f in $made) {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $img.Save([System.IO.Path]::ChangeExtension($f.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item $f.FullName -Force
    }
    Write-Output "$($r.n): exit $($p.ExitCode), $(@($made).Count) image(s)"
    if ($r.n -like 'pad_*') {
        $gs = Join-Path $vault '_settings\game_settings.json'
        if (Test-Path $gs) { Write-Output "  game_settings.json: $((Get-Content $gs -Raw) -replace '\s+', ' ')" } else { Write-Output '  game_settings.json: (none written)' }
        if (Test-Path $trace) {
            Get-Content $trace | Where-Object { $_ -like 'gs-*' } | ForEach-Object { Write-Output "  trace: $_" }
            Get-Content $trace | Where-Object { $_ -match '^pad (b|a) ' } | ForEach-Object { if ($_ -match '^pad (\S+) page=(\S+) popup=(\S+)') { Write-Output "  pad $($Matches[1]): page=$($Matches[2]) popup=$($Matches[3])" } }
        }
        $hj = "$box\userdata\hub.json"
        if ((Test-Path $hj) -and ((Get-Content $hj -Raw) -match '"option_sections":\s*"([^"]*)"')) { Write-Output "  hub.json option_sections: '$($Matches[1])'" } else { Write-Output '  hub.json option_sections: (no file or key)' }
    }
    if ($p.ExitCode -ne 0 -or @($made).Count -eq 0) { $fail++ }
}
Remove-Item -Recurse -Force $box
Write-Output "PNGs in $Out; $fail failure(s)"
exit ([int]($fail -gt 0))
