# Headless screenshots + pad walks of the dialogs converted to ui::dialog (ui_sheet.h), batch 2b: the couch party setup (and its game
# chips), Party night / The plan, the standings popup, your profile editor, the remove / block question, Plan a session (game night), the
# three Kanto Together dialogs and the admin "Are you sure?", plus the game night card's controller entry in the Friends list.
# No window is ever shown, everything is muted (SSB64_MUTE / RETROHUB_QA), no network (fake friends / room / admin data only; headless
# never connects), no game or emulator starts and no download runs.
#   powershell -NoProfile -ExecutionPolicy Bypass -File qa\dialogs2b_shots.ps1 [-Bin <build output dir>] [-Tag before|after] [-Out dir] [-Only regex] [-NoPad] [-NoStatic]
# -Bin is COPIED (without userdata) into build\qa_dlg2b_box_<tag> and run from there with a scratch vault; it is never run in place.
# Refuses to start while the owner is gaming (exit 3) and waits (re-checking every 60 s) if a game starts mid-run. Kills only its own PIDs.
# Output: <Out>\<tag>\<case>_<size>.png (+ -<shot>.png for pad walks), <case>.trace, summary.txt with the pad-walk checks.
param([string]$Bin = '', [string]$Tag = 'after', [string]$Out = '', [string]$Only = '.', [switch]$NoPad, [switch]$NoStatic)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_dialogs2b_out' }
$Out = Join-Path $Out $Tag
$box = Join-Path $root "build\qa_dlg2b_box_$Tag"

function Get-OwnerGames {
    Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
        Where-Object { -not ($_.Path -and ($_.Path -like '*\SmashClassicPort\wt\*' -or $_.Path -like '*\Temp\*')) }
}
if (Get-OwnerGames) { Write-Output "A game is running ($((Get-OwnerGames).Name -join ', ')): not starting."; exit 3 }
New-Item -ItemType Directory -Force $Out | Out-Null
if ($Only -eq '.') { Get-ChildItem $Out -File -ErrorAction SilentlyContinue | Remove-Item -Force }   # a full run starts clean; -Only reruns keep the rest
if (Test-Path $box) { Remove-Item -Recurse -Force $box }
New-Item -ItemType Directory -Force $box | Out-Null
Get-ChildItem $Bin | Where-Object { $_.Name -ne 'userdata' } | ForEach-Object { Copy-Item -Recurse -Force $_.FullName $box }
Add-Type -AssemblyName System.Drawing

$fr = 'HUB_SHOT_FAKE_FRIENDS=1;HUB_SHOT_FRIENDS=1'
$sizes = @(@('600', '1024x600', ''), @('720', '1280x720', ''), @('1080', '1920x1080', ''), @('600s150', '1024x600', 'HUB_UI_SCALE=150'))
$static = @(
    @{ n = 'couch';        env = 'HUB_SHOT_PARTY=couch' },
    @{ n = 'couchpick';    env = 'HUB_SHOT_PARTY=couchpick;HUB_SHOT_FAKE_LIB=30' },   # 30 made-up games: the setup's 24 game chips
    @{ n = 'partysetup';   env = 'HUB_SHOT_PARTY=setup' },
    @{ n = 'partyplan';    env = 'HUB_SHOT_PARTY=plan' },
    @{ n = 'partystand';   env = 'HUB_SHOT_PARTY=stand' },
    @{ n = 'profile';      env = "$fr;HUB_SHOT_PROFILES=1;HUB_SHOT_PROF_EDIT=1" },
    @{ n = 'frconf';       env = "$fr;HUB_SHOT_FRCONF=remove" },
    @{ n = 'frblock';      env = "$fr;HUB_SHOT_FRCONF=block" },
    @{ n = 'plannight';    env = "$fr;HUB_SHOT_FAKE_NIGHT=1;HUB_SHOT_PLAN_NIGHT=1" },
    @{ n = 'kt_setup';     env = 'HUB_SHOT_KT=installing' },
    @{ n = 'kt_intro';     env = 'HUB_SHOT_KT=intro' },
    @{ n = 'kt_howto';     env = 'HUB_SHOT_KT=howto' },
    @{ n = 'admconf';      env = 'HUB_SHOT_ADMIN=confirm' },
    @{ n = 'nightcard';    env = "$fr;HUB_SHOT_FAKE_NIGHT=1" }
)
# Pad walks: 'wait' = a pause step (the first one also makes the pad focus visible), shots on the way, B at the end. A is pressed only where
# it opens something or picks a harmless option (the night card's Dismiss: a scratch-vault file); online actions are skipped headless.
$d6 = (@('down') * 6) -join ','
$walks = @(
    @{ n = 'pad_couch';      size = '1280x720'; env = 'HUB_SHOT_PARTY=couch';                         pad = 'wait,shot:open,left,shot:moved,b,shot:closed' },
    @{ n = 'pad_couchpick';  size = '1024x600'; env = 'HUB_SHOT_PARTY=couchpick;HUB_SHOT_FAKE_LIB=30;HUB_UI_SCALE=150'; pad = "wait,shot:open,up,shot:u1,$d6,shot:d6,down,down,down,down,shot:d10,rt,shot:rt,b,shot:closed" },
    @{ n = 'pad_setup';      size = '1024x600'; env = 'HUB_SHOT_PARTY=setup;HUB_UI_SCALE=150';        pad = "wait,shot:open,$d6,shot:d6,down,down,down,down,shot:d10,b,shot:closed" },
    @{ n = 'pad_plan';       size = '1280x720'; env = 'HUB_SHOT_PARTY=plan';                          pad = 'wait,shot:open,b,shot:closed' },
    @{ n = 'pad_stand';      size = '1280x720'; env = 'HUB_SHOT_PARTY=stand';                         pad = 'wait,shot:open,b,shot:closed' },
    @{ n = 'pad_profile';    size = '1024x600'; env = "$fr;HUB_SHOT_PROFILES=1;HUB_SHOT_PROF_EDIT=1;HUB_UI_SCALE=150"; pad = "wait,shot:open,right,right,shot:right2,$d6,shot:d6,b,shot:closed" },
    @{ n = 'pad_frconf';     size = '1280x720'; env = "$fr;HUB_SHOT_FRCONF=remove";                   pad = 'wait,shot:open,right,shot:moved,b,shot:closed' },
    @{ n = 'pad_plannight';  size = '1024x600'; env = "$fr;HUB_SHOT_FAKE_NIGHT=1;HUB_SHOT_PLAN_NIGHT=1;HUB_UI_SCALE=150"; pad = "wait,shot:open,$d6,shot:d6,down,down,down,down,shot:d10,b,shot:closed" },
    # opened like a person would (a click on the chat header's "Plan a session" pill once the hub has settled; an aid that opens it on the
    # very first frames can lose its first focus to the library, see ui_sheet.h), then A opens the Game list, B closes only that, B the dialog
    @{ n = 'pad_planlist';   size = '1280x800'; env = "$fr;HUB_SHOT_CHAT=1;HUB_SHOT_CLICK=1153,40";      pad = 'wait,wait,wait,wait,wait,wait,wait,wait,wait,wait,wait,wait,shot:open,a,shot:list,b,shot:list_closed,b,shot:closed' },
    @{ n = 'pad_kthowto';    size = '1024x600'; env = 'HUB_SHOT_KT=howto;HUB_UI_SCALE=150';           pad = 'wait,shot:open,rt,shot:rt,rt,shot:rt2,b,shot:closed' },
    @{ n = 'pad_ktintro';    size = '1280x720'; env = 'HUB_SHOT_KT=intro';                            pad = 'wait,shot:open,b,shot:closed' },
    @{ n = 'pad_ktsetup';    size = '1280x720'; env = 'HUB_SHOT_KT=installing';                       pad = 'wait,shot:open,b,shot:after_b' },
    @{ n = 'pad_admconf';    size = '1280x720'; env = 'HUB_SHOT_ADMIN=confirm';                       pad = 'wait,shot:open,right,shot:moved,b,shot:closed' },
    # the game night card: View opens the Friends panel (the pad lands on its top), down to the card's row, A opens the small dialog
    # (nothing else happens), B closes it, A again, right to Dismiss, A dismisses (the row goes away)
    @{ n = 'pad_nightcard';  size = '1280x720'; env = 'HUB_SHOT_FAKE_FRIENDS=1;HUB_SHOT_FAKE_NIGHT=1';                   pad = 'wait,view,down,down,down,shot:row,a,shot:dialog,b,shot:closed,a,shot:again,right,shot:dismiss_focus,a,shot:dismissed' },
    @{ n = 'pad_nightcard150'; size = '1024x600'; env = 'HUB_SHOT_FAKE_FRIENDS=1;HUB_SHOT_FAKE_NIGHT=1;HUB_UI_SCALE=150'; pad = 'wait,view,down,down,down,shot:row,a,shot:dialog,b,shot:closed' }
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
    foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ($k -match '^(HUB_SHOT_|HUB_START_|HUB_SELECT_|HUB_SCREENSHOT|HUB_SCALE|HUB_UI_SCALE|HUB_TV|HUB_THEME|RETROHUB_)') { $psi.EnvironmentVariables.Remove($k) } }
    foreach ($kv in @("HUB_SCREENSHOT=$bmp", "HUB_SHOT_SIZE=$size", 'HUB_SHOT_SECS=4', "HUB_SHOT_VAULT=$vault", 'HUB_SHOT_FAKE_LIB=6', "HUB_SHOT_TRACE=$trace",
                      'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETRORUN_MUTE=1', 'N64RUN_MUTE=1', 'RETROHUB_KT_MUTE=1', 'RETROHUB_NO_CLOUD=1', 'RETROHUB_NO_SHELL=1') + ($envs -split ';')) {
        if ($kv -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; return "TIMEOUT" }   # our own PID only
    $made = @(Get-ChildItem $Out -Filter "$name*.bmp")
    foreach ($f in $made) {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $img.Save([System.IO.Path]::ChangeExtension($f.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item $f.FullName -Force
    }
    return "exit $($p.ExitCode), $($made.Count) image(s)"
}

function Rows($tf) {
    $rows = @()
    foreach ($l in @(Get-Content $tf | Where-Object { $_ -like 'pad *' })) {
        if ($l -match '^pad (\S+) page=(\S+) popup=(\d) friends=(\d) nav=([0-9a-f]+) at=(-?\d+),(-?\d+) .*sy=(\S+) win=(.*)$') {
            $rows += [pscustomobject]@{ tok = $Matches[1]; popup = [int]$Matches[3]; friends = [int]$Matches[4]; nav = $Matches[5]; x = [int]$Matches[6]; y = [int]$Matches[7]; sy = $Matches[8]; win = $Matches[9] }
        }
    }
    return $rows
}

$log = @()
$script:fail = 0
function Check($what, $ok, $detail) {
    $script:log += "    CHECK $what : $(if ($ok) { 'PASS' } else { "FAIL ($detail)" })"
    if (-not $ok) { $script:fail++ }
}

if (-not $NoStatic) {
    foreach ($c in $static) {
        foreach ($s in $sizes) {
            $n = "$($c.n)_$($s[0])"
            if ($n -notmatch $Only) { continue }
            $e = $c.env
            if ($s[2]) { $e = "$e;$($s[2])" }
            $r = Run-Hub $n $s[1] $e
            $log += "$n : $r"
            if ($r -notlike 'exit 0*') { $script:fail++ }
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
        if (-not (Test-Path $tf)) { $log += '    no trace'; $script:fail++; continue }
        $rows = @(Rows $tf)
        $other = @(Get-Content $tf | Where-Object { $_ -notlike 'pad *' })
        foreach ($x in $rows) { $log += "    $($x.tok) popup=$($x.popup) friends=$($x.friends) nav=$($x.nav) at=$($x.x),$($x.y) sy=$($x.sy) win=$($x.win)" }
        foreach ($l in $other) { $log += "    $l" }
        $b = @($rows | Where-Object { $_.tok -eq 'b' })
        $open = @($rows | Where-Object { $_.tok -eq 'shot:open' })
        switch ($w.n) {
            'pad_ktsetup' { Check 'B does nothing in the fake install (no cancel to make)' ($b.Count -and $b[-1].popup -eq 1) "popup=$(if ($b.Count) { $b[-1].popup })" }
            'pad_planlist' {
                Check 'the first B only closes the Game list' ($b.Count -ge 1 -and $b[0].popup -eq 1 -and $b[0].win -like '##plan_night*') "row=$(if ($b.Count) { "popup=$($b[0].popup) win=$($b[0].win)" })"
                Check 'the second B closes the dialog, the chat stays' ($b.Count -ge 2 -and $b[-1].popup -eq 0 -and $b[-1].win -eq '##friends') "row=$(if ($b.Count -ge 2) { "popup=$($b[-1].popup) win=$($b[-1].win)" })"
            }
            { $_ -like 'pad_nightcard*' } {
                Check 'the D-pad focuses the game night row' (@($other | Where-Object { $_ -like 'gn-card-focus *' }).Count -ge 1) 'no gn-card-focus line'
                $ia = @($rows | Where-Object { $_.tok -eq 'a' })
                Check 'A opens the card dialog' ($ia.Count -ge 1 -and $ia[0].popup -eq 1 -and $ia[0].win -like 'gn_card*') "row=$(if ($ia.Count) { "popup=$($ia[0].popup) win=$($ia[0].win)" })"
                $acts = @($other | Where-Object { $_ -like 'gn-card-act *' -or $_ -like 'gn-card-dismiss *' })
                $first = [array]::IndexOf([string[]](Get-Content $tf), ((Get-Content $tf) | Where-Object { $_ -like 'pad b *' } | Select-Object -First 1))
                $early = @((Get-Content $tf)[0..([Math]::Max(0, $first))] | Where-Object { $_ -like 'gn-card-act *' -or $_ -like 'gn-card-dismiss *' })
                Check 'opening the dialog and B fire nothing' ($early.Count -eq 0) ($early -join ' | ')
                Check 'B closes the dialog, the Friends panel stays' ($b.Count -ge 1 -and $b[0].popup -eq 0 -and $b[0].friends -eq 1 -and $b[0].win -like '##friends*') "row=$(if ($b.Count) { "popup=$($b[0].popup) friends=$($b[0].friends) win=$($b[0].win)" })"
                if ($w.n -eq 'pad_nightcard') { Check 'Dismiss from the dialog dismisses the card' ($acts.Count -eq 1 -and $acts[0] -like 'gn-card-dismiss *') ($acts -join ' | ') }
            }
            default { if ($b.Count) { Check 'B closes the dialog' ($b[-1].popup -eq 0) "popup=$($b[-1].popup)" } }
        }
        if ($open.Count) { $log += "    default focus: nav=$($open[0].nav) at=$($open[0].x),$($open[0].y) win=$($open[0].win)" }
    }
}
Remove-Item -Recurse -Force $box
$log += "failures: $($script:fail)"
$sumName = if ($Only -eq '.') { 'summary.txt' } else { 'summary-' + ($Only -replace '[^A-Za-z0-9_]', '_') + '.txt' }
$log | Set-Content (Join-Path $Out $sumName)
Write-Output "PNGs + traces in $Out; $($script:fail) failure(s)"
exit ([int]($script:fail -gt 0))
