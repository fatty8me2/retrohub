# Headless screenshots + pad walks of the dialogs converted to ui::dialog (ui_sheet.h), batch 2a: Customize and the shelf dialogs
# (customize_ui.cpp), a friend's game page and the pack Uninstall confirm (friend_games_ui.cpp), the username prompt (username_ui.cpp),
# the two account confirms (account_ui.cpp) and Find my games (find_games_ui.cpp).
# No window is ever shown, everything is muted (SSB64_MUTE / RETRORUN_MUTE / N64RUN_MUTE, RETROHUB_QA=1), no cloud (RETROHUB_NO_CLOUD), the
# shared-pack lookups go to a dead loopback port (RETROHUB_GP_BASE), fake friends only, no game or emulator starts and no download runs.
# The pad walks only move the focus and press B (never A on an action button; A only opens Customize / Add to shelf from the More menu).
#   powershell -NoProfile -ExecutionPolicy Bypass -File qa\dialogs2a_shots.ps1 [-Bin <build output dir>] [-Tag after] [-Out dir] [-Only regex] [-NoPad] [-NoStatic] [-NoFakeFind]
# -Bin is COPIED (without userdata / vault) into build\qa_dlg2a_box_<tag> and run from there with a scratch vault; never run in place.
# -NoFakeFind skips find_fake (HUB_SHOT_FINDGAMES=results WITHOUT a made-up profile). Pass it for any build older than this lane: before it,
#   that aid scanned the real profile folder of the PC. With this lane's build it shows made-up folders and never reads the disk.
# Refuses to start while the owner is gaming (exit 3) and waits (re-checking every 60 s) if a game starts mid-run. One hub at a time; a hub
# that hangs is ended by its PID.
# Output: <Out>\<tag>\<case>_<size>.png (+ -<shot>.png for pad walks), <case>.trace, summary.txt with the pad-walk checks.
param([string]$Bin = '', [string]$Tag = 'after', [string]$Out = '', [string]$Only = '.', [switch]$NoPad, [switch]$NoStatic, [switch]$NoFakeFind)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_dialogs2a_out' }
$Out = Join-Path $Out $Tag
$box = Join-Path $root "build\qa_dlg2a_box_$Tag"

function Get-OwnerGames {
    Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
        Where-Object { -not ($_.Path -and ($_.Path -like '*\SmashClassicPort\wt\*' -or $_.Path -like '*\Temp\*')) }
}
if (Get-OwnerGames) { Write-Output "A game is running ($((Get-OwnerGames).Name -join ', ')): not starting."; exit 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
if (Test-Path $box) { Remove-Item -Recurse -Force $box }
New-Item -ItemType Directory -Force $box | Out-Null
& robocopy $Bin $box /MIR /XD userdata vault /NFL /NDL /NJH /NJS /NP | Out-Null
if (-not (Test-Path (Join-Path $box 'RetroHub.exe'))) { throw "no RetroHub.exe in $box" }
Add-Type -AssemblyName System.Drawing

# A made-up profile for "Find my games" (HUB_FINDGAMES_HOME): one folder holds many consoles plus zips (its counts are the longest row),
# the rest are small, so the list overflows a short window.
$home2 = Join-Path $box 'fakehome'
$sets = @(@('Desktop\N64 Collection', 'z64'), @('Desktop\Game Boy Advance', 'gba'), @('Downloads\New games', 'z64'),
          @('Games\DS', 'nds'), @('Games\3DS', '3ds'), @('ROMs\PlayStation', 'cue'), @('Emulation\Homebrew GBA', 'gba'), @('Roms\Japan imports', 'sfc'))
$i = 0
foreach ($s in $sets) {
    $d = Join-Path $home2 $s[0]
    New-Item -ItemType Directory -Force $d | Out-Null
    for ($k = 1; $k -le 3; $k++) { Set-Content -Path (Join-Path $d "Test Game $i-$k.$($s[1])") -Value 'x' }
    $i++
}
$mixed = Join-Path $home2 'Documents\Emulation\Everything'
New-Item -ItemType Directory -Force $mixed | Out-Null
foreach ($x in @('z64', 'gba', 'gbc', 'gb', 'nds', 'sfc', 'nes', 'cue', 'iso', 'zip', 'zip', 'zip', 'zip', 'zip', 'zip', '7z')) {
    $i++
    Set-Content -Path (Join-Path $mixed "Test Game $i.$x") -Value 'x'
}

$lib = 'HUB_SHOT_FAKE_LIB=14'
$friends = 'HUB_SHOT_FAKE_FRIENDS=1;HUB_SHOT_FAKE_LIB=all;HUB_SHOT_FAKE_FRIEND_LIBS=40;HUB_START_VIEW=all;HUB_SHOT_GHOST_OPEN=Banjo-Tooie;HUB_SHOT_SECS=8'   # (an older build reads the title as index 0)
$sizes = @(@('600', '1024x600', ''), @('720', '1280x720', ''), @('1080', '1920x1080', ''), @('600s150', '1024x600', 'HUB_UI_SCALE=150'))
# l = layout (trad = Traditional, desk = Library desk)
$static = @(
    @{ n = 'cust_customize';  l = 'trad'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=customize" },
    @{ n = 'cust_shots';      l = 'trad'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=shots;HUB_SHOT_GALLERY=1" },
    @{ n = 'cust_picker';     l = 'trad'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=picker;HUB_SHOT_CUSTOM_SHELVES=12" },
    @{ n = 'cust_manager';    l = 'trad'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=manager;HUB_SHOT_CUSTOM_SHELVES=12" },
    @{ n = 'cust_smart';      l = 'trad'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=smart" },
    @{ n = 'ghost';           l = 'trad'; env = $friends },
    @{ n = 'ghost_inst';      l = 'trad'; env = "$friends;HUB_SHOT_GHOST_INSTALLABLE=1" },
    @{ n = 'uninst_trad';     l = 'trad'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_SHOT_GP_UNINSTALL=fake:2" },
    @{ n = 'uninst_desk';     l = 'desk'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_SHOT_GP_UNINSTALL=fake:2" },
    @{ n = 'username';        l = 'trad'; env = 'HUB_SHOT_USERNAME_PROMPT=1;HUB_SHOT_USERNAME=taken' },
    @{ n = 'username_many';   l = 'trad'; env = 'HUB_SHOT_USERNAME_PROMPT=1;HUB_SHOT_USERNAME=notallowed' },
    @{ n = 'acct_stop';       l = 'trad'; env = 'HUB_SHOT_PAGE=settings:account;HUB_SHOT_ACCOUNT=pcs_confirm' },
    @{ n = 'acct_replace';    l = 'trad'; env = 'HUB_SHOT_PAGE=settings:account;HUB_SHOT_ACCOUNT=confirm' },
    @{ n = 'find_results';    l = 'trad'; env = "HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FINDGAMES=results;HUB_FINDGAMES_HOME=$home2" },
    @{ n = 'find_fake';       l = 'trad'; env = 'HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FINDGAMES=results' },
    @{ n = 'find_scanning';   l = 'trad'; env = 'HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FINDGAMES=scanning' }
)
# Pad walks: wait = a press that only wakes the pad focus (shows the default), shots on the way, B at the end.
# expect = a regex the trace line of the LAST B press must match (where the focus went back to, and that the page is still there).
$d6 = (@('down') * 6) -join ','
$d10 = (@('down') * 10) -join ','
$tradMore = 'right,right,a,down,down,down,down,down'   # Play -> More -> its menu -> Customize (fake:2, a GameCube game)
$deskMore = 'down,right,right,right,a,down,down,down,down,down'
$onPage = 'win=details##uisheet'
$onDesk = '^pad b page=library popup=0 .*win=\S*##desk_pane'   # back in the Library desk's game page pane, nothing open over it
$walks = @(
    @{ n = 'pad_cust_trad';    l = 'trad'; size = '1280x720'; env = "$lib;HUB_SHOT_DETAILS=fake:2"; pad = "$tradMore,a,shot:open,$d6,shot:d6,$d6,shot:d12,b,shot:closed"; expect = $onPage },
    @{ n = 'pad_cust_desk';    l = 'desk'; size = '1280x720'; env = $lib; pad = "$deskMore,a,shot:open,$d6,shot:d6,$d6,shot:d12,b,shot:closed"; expect = $onDesk },
    @{ n = 'pad_cust_s150';    l = 'trad'; size = '1024x600'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_UI_SCALE=150"; pad = "$tradMore,a,shot:open,$d6,shot:d6,$d6,shot:d12,b,shot:closed"; expect = $onPage },
    @{ n = 'pad_shelf_trad';   l = 'trad'; size = '1280x720'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_SHOT_CUSTOM_SHELVES=12"; pad = "$tradMore,down,a,shot:open,$d10,shot:d10,b,shot:closed"; expect = $onPage },
    @{ n = 'pad_shelf_desk';   l = 'desk'; size = '1280x720'; env = "$lib;HUB_SHOT_CUSTOM_SHELVES=12"; pad = "$deskMore,down,a,shot:open,$d10,shot:d10,b,shot:closed"; expect = $onDesk },
    @{ n = 'pad_shots';        l = 'trad'; size = '1280x720'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=shots;HUB_SHOT_GALLERY=1"; pad = 'wait,shot:open,right,shot:right,down,shot:down,down,shot:cancel,b,shot:closed'; expect = 'win=Customize##cust' },
    @{ n = 'pad_shots_s150';   l = 'trad'; size = '1024x600'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=shots;HUB_SHOT_GALLERY=1;HUB_UI_SCALE=150"; pad = 'wait,shot:open,down,shot:down,down,shot:down2,b,shot:closed'; expect = 'win=Customize##cust' },
    @{ n = 'pad_picker_s150';  l = 'trad'; size = '1024x600'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=picker;HUB_SHOT_CUSTOM_SHELVES=12;HUB_UI_SCALE=150"; pad = "wait,shot:open,$d10,shot:d10,down,down,down,down,shot:d14,b,shot:closed"; expect = 'popup=0' },
    @{ n = 'pad_manager_s150'; l = 'trad'; size = '1024x600'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=manager;HUB_SHOT_CUSTOM_SHELVES=12;HUB_UI_SCALE=150"; pad = "wait,shot:open,$d10,shot:d10,$d10,shot:d20,b,shot:closed"; expect = 'popup=0' },
    @{ n = 'pad_smart_s150';   l = 'trad'; size = '1024x600'; env = "$lib;HUB_SELECT_GAME=fake:2;HUB_SHOT_CUSTOM=smart;HUB_UI_SCALE=150"; pad = "wait,shot:open,$d6,shot:d6,$d6,shot:d12,b,shot:closed"; expect = 'popup=0' },
    @{ n = 'pad_ghost_s150';   l = 'trad'; size = '1024x600'; env = "$friends;HUB_UI_SCALE=150"; pad = "wait,shot:open,$d6,shot:d6,$d6,shot:d12,b,shot:closed"; expect = 'popup=0' },
    @{ n = 'pad_ghost_up';     l = 'trad'; size = '1024x600'; env = "$friends;HUB_UI_SCALE=150"; pad = 'wait,shot:open,up,shot:up1,up,shot:up2,rt,shot:rt,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_ghost_up720';  l = 'trad'; size = '1280x720'; env = $friends; pad = 'wait,shot:open,up,shot:up,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_ghost_inst';   l = 'trad'; size = '1280x720'; env = "$friends;HUB_SHOT_GHOST_INSTALLABLE=1"; pad = 'wait,shot:open,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_uninst_trad';  l = 'trad'; size = '1280x720'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_SHOT_GP_UNINSTALL=fake:2"; pad = 'wait,shot:open,right,shot:moved,b,shot:closed'; expect = $onPage },
    # Uninstall pressed by pad (Play -> down to the last item of About, Uninstall; A opens the confirm; B = Keep it puts the focus back on Uninstall)
    @{ n = 'pad_uninst_ptrad'; l = 'trad'; size = '1280x720'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_SHOT_GP_UNINSTALL=fake:2:pad"; pad = 'down,down,down,down,down,down,down,shot:at,a,shot:open,right,shot:moved,b,shot:closed'; expect = $onPage },
    @{ n = 'pad_uninst_pdesk'; l = 'desk'; size = '1280x720'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_SHOT_GP_UNINSTALL=fake:2:pad"; pad = 'down,down,down,down,down,down,down,shot:at,a,shot:open,right,shot:moved,b,shot:closed'; expect = $onDesk },
    # The friend's game page: RT pages the middle while the focus stays on the pinned buttons, then Up reaches the friend rows ("Ask ... to host")
    @{ n = 'pad_ghost_rt';     l = 'trad'; size = '1024x600'; env = "$friends;HUB_UI_SCALE=150"; pad = 'wait,shot:open,up,shot:up0,rt,shot:rt1,rt,shot:rt2,up,shot:up,up,shot:up2,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_uninst_desk';  l = 'desk'; size = '1280x720'; env = "$lib;HUB_SHOT_DETAILS=fake:2;HUB_SHOT_GP_UNINSTALL=fake:2"; pad = 'wait,shot:open,right,shot:moved,b,shot:closed'; expect = '^pad b page=library popup=0' },
    @{ n = 'pad_username';     l = 'trad'; size = '1280x720'; env = 'HUB_SHOT_USERNAME_PROMPT=1;HUB_SHOT_USERNAME=taken'; pad = 'wait,shot:open,down,shot:d1,down,shot:d2,b,shot:closed'; expect = 'popup=0' },
    # opened after start-up (=late) like a real prompt: the first focus is the name box (never the title row's X)
    @{ n = 'pad_username_late'; l = 'trad'; size = '1280x720'; env = 'HUB_SHOT_USERNAME_PROMPT=late;HUB_SHOT_USERNAME=taken'; pad = 'wait,wait,wait,wait,wait,shot:open,down,shot:d1,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_username_s150'; l = 'trad'; size = '1024x600'; env = 'HUB_SHOT_USERNAME_PROMPT=1;HUB_SHOT_USERNAME=notallowed;HUB_UI_SCALE=150'; pad = "wait,shot:open,$d6,shot:d6,b,shot:closed"; expect = 'popup=0' },
    @{ n = 'pad_acct_stop';    l = 'trad'; size = '1280x720'; env = 'HUB_SHOT_PAGE=settings:account;HUB_SHOT_ACCOUNT=pcs_confirm'; pad = 'wait,shot:open,right,shot:moved,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_acct_replace'; l = 'trad'; size = '1280x720'; env = 'HUB_SHOT_PAGE=settings:account;HUB_SHOT_ACCOUNT=confirm'; pad = 'wait,shot:open,right,shot:moved,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_acct_s150';    l = 'trad'; size = '1024x600'; env = 'HUB_SHOT_PAGE=settings:account;HUB_SHOT_ACCOUNT=confirm;HUB_UI_SCALE=150'; pad = 'wait,shot:open,right,shot:moved,b,shot:closed'; expect = 'popup=0' },
    @{ n = 'pad_find_s150';    l = 'trad'; size = '1024x600'; env = "HUB_SHOT_FAKE_LIB=6;HUB_SHOT_FINDGAMES=results;HUB_FINDGAMES_HOME=$home2;HUB_UI_SCALE=150"; pad = "wait,shot:open,$d10,shot:d10,b,shot:closed"; expect = 'popup=0' }
)

function Run-Hub($name, $size, $layout, $envs) {
    while (Get-OwnerGames) { Write-Output "owner is gaming ($((Get-OwnerGames).Name -join ',')): waiting 60 s"; Start-Sleep -Seconds 60 }
    if (Test-Path "$box\userdata") { Remove-Item -Recurse -Force "$box\userdata" }
    $vault = Join-Path $box 'vault'
    if (Test-Path $vault) { Remove-Item -Recurse -Force $vault }
    New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
    $bmp = Join-Path $Out "$name.bmp"
    $trace = Join-Path $Out "$name.trace"
    Remove-Item $trace -ErrorAction SilentlyContinue
    Get-ChildItem $Out -Filter "$name*.png" -ErrorAction SilentlyContinue | Where-Object { $_.BaseName -eq $name -or $_.BaseName.StartsWith("$name-") } | Remove-Item -Force
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $box 'RetroHub.exe'
    $psi.WorkingDirectory = $box
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ($k -like 'HUB_*') { $psi.EnvironmentVariables.Remove($k) } }
    $lay = if ($layout -eq 'desk') { 'desk' } else { 'traditional' }
    foreach ($kv in @("HUB_SCREENSHOT=$bmp", "HUB_SHOT_SIZE=$size", 'HUB_SHOT_SECS=5', "HUB_SHOT_VAULT=$vault", "HUB_SHOT_TRACE=$trace", "HUB_SHOT_LAYOUT=$lay",
                      'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETRORUN_MUTE=1', 'N64RUN_MUTE=1', 'RETROHUB_NO_CLOUD=1', 'RETROHUB_NO_SHELL=1',
                      'RETROHUB_GP_BASE=127.0.0.1:9') + ($envs -split ';')) {
        if ($kv -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; return 'TIMEOUT' }
    $made = @(Get-ChildItem $Out -Filter "$name*.bmp" | Where-Object { $_.BaseName -eq $name -or $_.BaseName.StartsWith("$name-") })
    foreach ($f in $made) {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $img.Save([System.IO.Path]::ChangeExtension($f.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item $f.FullName -Force
    }
    return "exit $($p.ExitCode), $($made.Count) image(s)"
}

$log = @()
$fail = 0
if (-not $NoStatic) {
    foreach ($c in $static) {
        if ($NoFakeFind -and $c.n -eq 'find_fake') { continue }
        foreach ($s in $sizes) {
            $n = "$($c.n)_$($s[0])"
            if ($n -notmatch $Only) { continue }
            $e = $c.env
            if ($s[2]) { $e = "$e;$($s[2])" }
            $r = Run-Hub $n $s[1] $c.l $e
            $log += "$n : $r"
            if ($r -notlike 'exit 0*') { $fail++ }
            Write-Output "$n : $r"
            $tf = Join-Path $Out "$n.trace"
            if (Test-Path $tf) { foreach ($l in @(Get-Content $tf | Where-Object { $_ -like 'findgames*' -or $_ -like 'captured*' })) { $log += "    $l" } }
        }
    }
}
if (-not $NoPad) {
    foreach ($w in $walks) {
        if ($w.n -notmatch $Only) { continue }
        $r = Run-Hub $w.n $w.size $w.l "$($w.env);HUB_SHOT_PAD=$($w.pad)"
        $log += "$($w.n) : $r"
        Write-Output "$($w.n) : $r"
        $tf = Join-Path $Out "$($w.n).trace"
        if (Test-Path $tf) {
            $rows = @(Get-Content $tf | Where-Object { $_ -like 'pad *' })
            foreach ($l in $rows) { if ($l -match '^pad (\S+) page=(\S+) popup=(\d) .*nav=([0-9a-f]+) at=(-?\d+),(-?\d+) osk=(\d).*sy=(\S+) win=(.*)$') { $log += "    $($Matches[1]) page=$($Matches[2]) popup=$($Matches[3]) nav=$($Matches[4]) at=$($Matches[5]),$($Matches[6]) osk=$($Matches[7]) sy=$($Matches[8]) win=$($Matches[9])" } }
            foreach ($l in @(Get-Content $tf | Where-Object { $_ -notlike 'pad *' -and $_ -notlike 'act *' })) { $log += "    $l" }
            $b = @($rows | Where-Object { $_ -like 'pad b *' })
            if ($w.expect -and $b.Count) {
                $ok = $b[-1] -match $w.expect
                $log += "    CHECK last B -> /$($w.expect)/: $(if ($ok) { 'PASS' } else { 'FAIL' })"
                if (-not $ok) { $fail++ }
            }
        }
    }
}
Remove-Item -Recurse -Force $box
$log += "failures: $fail"
$log | Set-Content (Join-Path $Out 'summary.txt')
Write-Output "PNGs + traces in $Out; $fail failure(s)"
exit ([int]($fail -gt 0))
