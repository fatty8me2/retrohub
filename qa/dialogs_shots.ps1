# Headless screenshots + pad walks of the dialogs converted to ui::dialog (ui_sheet.h), batch 1: the emulator / launch prompts
# (emu_modal_begin), Add games, Find my games, the ROM hack dialogs and the cloud save dialogs. No window is ever shown, nothing is muted
# by hand (SSB64_MUTE / RETROHUB_QA), no network (RETROHUB_NO_CLOUD), no game or emulator starts and no download runs: the pad walks only
# move the focus and press B (never A on an action button).
#   powershell -NoProfile -ExecutionPolicy Bypass -File qa\dialogs_shots.ps1 [-Bin <build output dir>] [-Tag after] [-Out dir] [-Only regex] [-NoPad] [-NoStatic]
# -Bin is COPIED (without userdata) into build\qa_dlg_box_<tag> and run from there with a scratch vault; it is never run in place.
# Refuses to start while the owner is gaming (exit 3) and waits (re-checking every 60 s) if a game starts mid-run.
# Output: <Out>\<tag>\<case>_<size>.png (+ -<shot>.png for pad walks), <case>.trace, summary.txt with the pad-walk checks.
param([string]$Bin = '', [string]$Tag = 'after', [string]$Out = '', [string]$Only = '.', [switch]$NoPad, [switch]$NoStatic)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_dialogs_out' }
$Out = Join-Path $Out $Tag
$box = Join-Path $root "build\qa_dlg_box_$Tag"

function Get-OwnerGames {
    Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
        Where-Object { -not ($_.Path -and ($_.Path -like '*\SmashClassicPort\wt\*' -or $_.Path -like '*\Temp\*')) }
}
if (Get-OwnerGames) { Write-Output "A game is running ($((Get-OwnerGames).Name -join ', ')): not starting."; exit 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
if (Test-Path $box) { Remove-Item -Recurse -Force $box }
New-Item -ItemType Directory -Force $box | Out-Null
Get-ChildItem $Bin | Where-Object { $_.Name -ne 'userdata' } | ForEach-Object { Copy-Item -Recurse -Force $_.FullName $box }
Add-Type -AssemblyName System.Drawing

# A made-up profile for "Find my games" (HUB_FINDGAMES_HOME): many small game folders so the list overflows a short window.
$home2 = Join-Path $box 'fakehome'
$sets = @(@('Desktop\N64 Collection', 'z64'), @('Desktop\Game Boy Advance', 'gba'), @('Desktop\Old backups\GameCube discs', 'iso'),
          @('Documents\Emulation\SNES', 'sfc'), @('Documents\Emulation\NES', 'nes'), @('Documents\Emulation\Game Boy Color', 'gbc'),
          @('Downloads\New games', 'z64'), @('Games\DS', 'nds'), @('Games\3DS', '3ds'), @('ROMs\PlayStation', 'cue'),
          @('ROMs\PSP', 'cso'), @('Emulation\Sega free', 'z64'), @('Emulation\Homebrew GBA', 'gba'), @('Roms\Japan imports', 'sfc'))
$i = 0
foreach ($s in $sets) {
    $d = Join-Path $home2 $s[0]
    New-Item -ItemType Directory -Force $d | Out-Null
    for ($k = 1; $k -le 3; $k++) { Set-Content -Path (Join-Path $d "Test Game $i-$k.$($s[1])") -Value 'x' }
    $i++
}

$sizes = @(@('600', '1024x600', ''), @('720', '1280x720', ''), @('1080', '1920x1080', ''), @('600s150', '1024x600', 'HUB_UI_SCALE=150'))
$static = @(
    @{ n = 'emu_azahar';      env = 'HUB_SHOT_EMU=azahar' },
    @{ n = 'emu_cemu';        env = 'HUB_SHOT_EMU=cemu' },
    @{ n = 'emu_working';     env = 'HUB_SHOT_EMU=working:azahar:42' },
    @{ n = 'emu_failed';      env = 'HUB_SHOT_EMU=failed' },
    @{ n = 'emu_unzip';       env = 'HUB_SHOT_EMU=unzip;HUB_SELECT_GAME=fake:1' },
    @{ n = 'emu_ps2bios';     env = 'HUB_SHOT_EMU=ps2bios' },
    @{ n = 'emu_ps1bios';     env = 'HUB_SHOT_EMU=ps1bios' },
    @{ n = 'emu_switchkeys';  env = 'HUB_SHOT_EMU=switchkeys' },
    @{ n = 'emu_hookshotav1'; env = 'HUB_SHOT_EMU=hookshotav1' },
    @{ n = 'emu_hookmissing'; env = 'HUB_SHOT_EMU=hookshotmissing' },
    @{ n = 'emu_missingrom';  env = 'HUB_SHOT_EMU=missingrom' },
    @{ n = 'emu_hidden';      env = 'HUB_SHOT_EMU=hidden;HUB_SHOT_HIDDEN=1' },
    @{ n = 'emu_hiddenmany';  env = 'HUB_SHOT_EMU=hidden;HUB_SHOT_HIDDEN=many' },
    @{ n = 'emu_resume';      env = 'HUB_SHOT_EMU=resume' },
    @{ n = 'emu_lichiiupd';   env = 'HUB_SHOT_EMU=lichiiupdate' },
    @{ n = 'emu_fritchii';    env = 'HUB_SHOT_EMU=fritchii' },
    @{ n = 'emu_nxprep';      env = 'HUB_SHOT_NXPREP=1' },
    @{ n = 'add_dialog';      env = 'HUB_SHOT_ADDGAMES=dialog' },
    @{ n = 'add_progress';    env = 'HUB_SHOT_ADDGAMES=progress' },
    @{ n = 'add_results';     env = 'HUB_SHOT_ADDGAMES=results' },
    @{ n = 'find_results';    env = "HUB_SHOT_FINDGAMES=results;HUB_FINDGAMES_HOME=$home2" },
    @{ n = 'find_scanning';   env = 'HUB_SHOT_FINDGAMES=scanning' },
    @{ n = 'hack_add';        env = 'HUB_SHOT_HACKS=add' },
    @{ n = 'hack_choose';     env = 'HUB_SHOT_HACKS=choose' },
    @{ n = 'hack_fail';       env = 'HUB_SHOT_HACKS=fail' },
    @{ n = 'hack_remove';     env = 'HUB_SHOT_HACKS=remove' },
    @{ n = 'cloud_conflict';  env = 'HUB_SHOT_CLOUD_DIALOG=conflict' },
    @{ n = 'cloud_conflict8'; env = 'HUB_SHOT_CLOUD_DIALOG=conflictmany' },
    @{ n = 'cloud_lease';     env = 'HUB_SHOT_CLOUD_DIALOG=lease' },
    @{ n = 'cloud_checking';  env = 'HUB_SHOT_CLOUD_DIALOG=checking' },
    @{ n = 'cloud_waiting';   env = 'HUB_SHOT_CLOUD_DIALOG=waiting' },
    @{ n = 'cloud_pulled';    env = 'HUB_SHOT_CLOUD_DIALOG=pulled' },
    @{ n = 'cloud_offline';   env = 'HUB_SHOT_CLOUD_DIALOG=offline' }
)
# Pad walks: wait = the first press (wakes the pad focus, shows the default), shots on the way, B at the end. Never A on an action.
$d10 = (@('down') * 10) -join ','
$bkText = 'Could not back up your save (the save file is in use). Cheats are on anyway.'   # cheats_ui.cpp's two notices, with made-up errors
$rsText = 'Could not restore: the backup folder is missing'
$walks = @(
    @{ n = 'pad_azahar';      size = '1280x720'; env = 'HUB_SHOT_EMU=azahar';                 pad = 'wait,shot:open,right,shot:moved,b,shot:closed' },
    @{ n = 'pad_ps1bios';     size = '1280x720'; env = 'HUB_SHOT_EMU=ps1bios';                pad = 'wait,shot:open,down,down,shot:moved,b,shot:closed' },
    # qa\pad_nav.ps1 step 12: down,left lands on Choose, A opens the pad folder browser (reads folder names only), Back closes it, B the prompt
    @{ n = 'pad_ps1browse';   size = '1280x800'; env = 'HUB_SHOT_EMU=ps1bios;HUB_SHOT_FAKE_LIB=14'; pad = 'shot:b0,down,left,a,shot:b_browser,back,shot:b_prompt,b,shot:b_closed' },
    @{ n = 'pad_ps2browse';   size = '1280x800'; env = 'HUB_SHOT_EMU=ps2bios;HUB_SHOT_FAKE_LIB=14'; pad = 'shot:b0,down,left,a,shot:b_browser,back,shot:b_prompt,b,shot:b_closed' },
    @{ n = 'pad_missingrom';  size = '1024x600'; env = 'HUB_SHOT_EMU=missingrom;HUB_UI_SCALE=150'; pad = 'wait,shot:open,down,down,down,shot:moved,b,shot:closed' },
    @{ n = 'pad_hiddenmany';  size = '1024x600'; env = 'HUB_SHOT_EMU=hidden;HUB_SHOT_HIDDEN=many;HUB_UI_SCALE=150'; pad = "wait,shot:open,$d10,shot:d10,down,down,down,down,shot:d14,rt,shot:rt,b,shot:closed" },
    @{ n = 'pad_resume';      size = '1280x720'; env = 'HUB_SHOT_EMU=resume';                 pad = 'wait,shot:open,down,shot:moved,b,shot:closed' },
    @{ n = 'pad_working';     size = '1280x720'; env = 'HUB_SHOT_EMU=working:azahar:42';      pad = 'wait,shot:open,b,shot:after_b' },
    @{ n = 'pad_addgames';    size = '1024x600'; env = 'HUB_SHOT_ADDGAMES=dialog;HUB_UI_SCALE=150'; pad = 'wait,shot:open,down,down,down,down,shot:moved,b,shot:closed' },
    @{ n = 'pad_findgames';   size = '1024x600'; env = "HUB_SHOT_FINDGAMES=results;HUB_FINDGAMES_HOME=$home2;HUB_UI_SCALE=150"; pad = "wait,shot:open,$d10,shot:d10,down,down,down,down,down,shot:d15,b,shot:closed" },
    @{ n = 'pad_hackchoose';  size = '1024x600'; env = 'HUB_SHOT_HACKS=choose;HUB_UI_SCALE=150'; pad = "wait,shot:open,$d10,shot:d10,down,down,down,down,down,down,shot:d16,b,shot:closed" },
    @{ n = 'pad_hackremove';  size = '1280x720'; env = 'HUB_SHOT_HACKS=remove';               pad = 'wait,shot:open,down,shot:moved,b,shot:closed' },
    @{ n = 'pad_conflict';    size = '1280x720'; env = 'HUB_SHOT_CLOUD_DIALOG=conflict';      pad = 'wait,shot:open,right,shot:moved,b,shot:closed' },
    @{ n = 'pad_conflict8';   size = '1024x600'; env = 'HUB_SHOT_CLOUD_DIALOG=conflictmany;HUB_UI_SCALE=150'; pad = 'wait,shot:open,rt,shot:rt,rt,shot:rt2,down,shot:moved,b,shot:closed' },
    @{ n = 'pad_lease';       size = '1280x720'; env = 'HUB_SHOT_CLOUD_DIALOG=lease';         pad = 'wait,shot:open,right,shot:moved,b,shot:closed' },
    @{ n = 'pad_waiting';     size = '1280x720'; env = 'HUB_SHOT_CLOUD_DIALOG=waiting'; pad = 'wait,shot:open,b,shot:after_b' },
    # The pad folder browser lists made-up entries in a screenshot run; choosing one of its folders is only traced, nothing is handed back
    # (the made-up C:\ is this PC's C:\: Add games would add it and scan it). Reached the way pad_ps1browse does (the PlayStation BIOS
    # prompt's Choose); the folder chosen is the made-up Desktop shortcut, a path this PC does not have, so an older build finds nothing
    # there either. NEVER walk to Add games' "Choose game files...": it opens the Windows file dialog even in a screenshot run (a visible
    # window).
    @{ n = 'pad_madeupfolder'; size = '1280x800'; env = 'HUB_SHOT_EMU=ps1bios;HUB_SHOT_FAKE_LIB=14'; pad = 'shot:b0,down,left,a,shot:b_browser,a,shot:made_up_folder,a,shot:picked,b,shot:b_closed' },
    # The Cheats dialog's own notices (HUB_SHOT_TOAST*, at 2.5 s) show while it is open: beside it when a spot fits, over it otherwise
    @{ n = 'pad_cheatstoast';    size = '1280x720'; env = "HUB_SHOT_CHEATS=list;HUB_SHOT_TOAST=$bkText;HUB_SHOT_TOAST_BAD=$rsText";                   pad = 'wait,wait,wait,wait,shot:toasts,b,shot:closed' },
    @{ n = 'pad_cheatstoast150'; size = '1024x600'; env = "HUB_SHOT_CHEATS=list;HUB_SHOT_TOAST=$bkText;HUB_SHOT_TOAST_BAD=$rsText;HUB_UI_SCALE=150"; pad = 'wait,wait,wait,wait,shot:toasts,b,shot:closed' }
)

function Run-Hub($name, $size, $envs) {
    while (Get-OwnerGames) { Write-Output "owner is gaming ($((Get-OwnerGames).Name -join ',')): waiting 60 s"; Start-Sleep -Seconds 60 }
    if (Test-Path "$box\userdata") { Remove-Item -Recurse -Force "$box\userdata" }
    $vault = Join-Path $box 'vault'
    if (Test-Path $vault) { Remove-Item -Recurse -Force $vault }
    New-Item -ItemType Directory -Force $vault | Out-Null
    $bmp = Join-Path $Out "$name.bmp"
    $trace = Join-Path $Out "$name.trace"
    Remove-Item $trace -ErrorAction SilentlyContinue
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $box 'RetroHub.exe'
    $psi.WorkingDirectory = $box
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    foreach ($kv in @("HUB_SCREENSHOT=$bmp", "HUB_SHOT_SIZE=$size", 'HUB_SHOT_SECS=4', "HUB_SHOT_VAULT=$vault", 'HUB_SHOT_FAKE_LIB=6', "HUB_SHOT_TRACE=$trace",
                      'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETRORUN_MUTE=1', 'N64RUN_MUTE=1', 'RETROHUB_NO_CLOUD=1', 'RETROHUB_NO_SHELL=1') + ($envs -split ';')) {
        if ($kv -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; return "TIMEOUT" }
    $made = @(Get-ChildItem $Out -Filter "$name*.bmp")
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
        foreach ($s in $sizes) {
            $n = "$($c.n)_$($s[0])"
            if ($n -notmatch $Only) { continue }
            $e = $c.env
            if ($s[2]) { $e = "$e;$($s[2])" }
            $r = Run-Hub $n $s[1] $e
            $log += "$n : $r"
            if ($r -notlike 'exit 0*') { $fail++ }
            Write-Output "$n : $r"
        }
    }
}
if (-not $NoPad) {
    foreach ($w in $walks) {
        if ($w.n -notmatch $Only) { continue }
        $r = Run-Hub $w.n $w.size "$($w.env);HUB_SHOT_PAD=$($w.pad)"
        $log += "$($w.n) : $r"
        Write-Output "$($w.n) : $r"
        $tf = Join-Path $Out "$($w.n).trace"
        if (Test-Path $tf) {
            $rows = @(Get-Content $tf | Where-Object { $_ -like 'pad *' })
            foreach ($l in $rows) { if ($l -match '^pad (\S+) .*popup=(\d) .*nav=([0-9a-f]+) at=(-?\d+),(-?\d+) osk=(\d).*sy=(\S+) win=(.*)$') { $log += "    $($Matches[1]) popup=$($Matches[2]) nav=$($Matches[3]) at=$($Matches[4]),$($Matches[5]) osk=$($Matches[6]) sy=$($Matches[7]) win=$($Matches[8])" } }
            foreach ($l in @(Get-Content $tf | Where-Object { $_ -notlike 'pad *' })) { $log += "    $l" }
            $b = @($rows | Where-Object { $_ -like 'pad b *' })
            if ($b.Count -and $w.n -notin @('pad_working', 'pad_waiting')) {
                $closed = $b[-1] -match 'popup=0'
                $log += "    CHECK B closes: $(if ($closed) { 'PASS' } else { 'FAIL' })"
                if (-not $closed) { $fail++ }
            }
            if ($w.n -eq 'pad_madeupfolder') {
                $tl = @(Get-Content $tf)
                $pick = @($tl | Where-Object { $_ -like 'osk-folder-pick *' })
                $used = @($tl | Where-Object { $_ -like 'notice *' })   # what the prompt did with a folder handed back (e.g. "No PlayStation BIOS ... in that folder.")
                $ok = $pick.Count -ge 1 -and $used.Count -eq 0
                $log += "    CHECK a made-up folder choice is only traced (nothing handed back): $(if ($ok) { 'PASS' } else { 'FAIL (' + (($pick + $used) -join ' | ') + ')' })"
                if (-not $ok) { $fail++ }
            }
            if ($w.n -like 'pad_cheatstoast*') {   # each notice is drawn while the dialog is open (before the walk's B), never held back
                $tl = @(Get-Content $tf)
                $iB = [array]::FindIndex([string[]]$tl, [Predicate[string]]{ param($s) $s -like 'pad shot:toasts *' })
                foreach ($txt in @($bkText, $rsText)) {
                    $i = [array]::FindIndex([string[]]$tl, [Predicate[string]]{ param($s) $s -like "toast-drawn * $txt" })
                    $ok = $i -ge 0 -and $iB -ge 0 -and $i -lt $iB
                    $spot = if ($i -ge 0) { ($tl[$i] -split ' ')[1] } else { 'never' }
                    $log += "    CHECK '$txt' shows while Cheats is open (spot: $spot): $(if ($ok) { 'PASS' } else { "FAIL (drawn at line $i, B at line $iB)" })"
                    if (-not $ok) { $fail++ }
                }
                $held = @($tl | Where-Object { $_ -like 'toast-held *' })
                $log += "    CHECK no notice held back under the dialog: $(if (-not $held.Count) { 'PASS' } else { 'FAIL (' + ($held -join ' | ') + ')' })"
                if ($held.Count) { $fail++ }
            }
        }
    }
    # The hub goes idle with a dialog open once its notice has gone (a notice held under a dialog never expired and kept every loop
    # drawing): Cheats open, a 2 s notice, the idle throttle on as if behind a game (HUB_IDLE_SIM), frames presented per second measured
    if ('idle_cheatstoast' -match $Only) {
        $rep = Join-Path $Out 'idle_cheatstoast.fps.txt'
        Remove-Item $rep -ErrorAction SilentlyContinue
        $r = Run-Hub 'idle_cheatstoast' '1280x720' "HUB_SHOT_CHEATS=list;HUB_SHOT_TOAST=$bkText;HUB_SHOT_TOAST_SECS=2;HUB_SHOT_SECS=16;HUB_SHOT_NOWAIT=1;HUB_IDLE_SIM=bg,noraster;HUB_FPS_REPORT=$rep"
        $log += "idle_cheatstoast : $r"
        Write-Output "idle_cheatstoast : $r"
        $line = if (Test-Path $rep) { (Get-Content $rep -Raw).Trim() } else { '' }
        $log += "    $line"
        $ok = $line -match 'steady \(after 3 s\): secs=[\d.]+ presented_per_s=([\d.]+)' -and [double]$Matches[1] -lt 30
        $log += "    CHECK idle with Cheats open after the notice expired (frames/s in steady state under 30): $(if ($ok) { 'PASS' } else { 'FAIL' })"
        if (-not $ok) { $fail++ }
    }
}
Remove-Item -Recurse -Force $box
$log += "failures: $fail"
$log | Set-Content (Join-Path $Out 'summary.txt')
Write-Output "PNGs + traces in $Out; $fail failure(s)"
exit ([int]($fail -gt 0))
