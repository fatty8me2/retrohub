# Headless gamepad walk onto the last controls a controller could not reach (ui2-padspots lane):
#   1. toast: the action of a toast ("Leave and join") with Y, 2. myprof: your own profile entry at the top of the Friends panel,
#   3. avatar: the avatar cells in the profile editor, 4. react: room chat reactions, 5. sort: reversing the library sort (X on the
#   focused Sort chip; sortkey: the Menu key on a keyboard).
# Each case walks onto the control, saves shots (<case>-<name>.png) and checks the HUB_SHOT_TRACE lines.
# No visible window, muted, no network, scratch vault only, fake friends / fake room only. Runs from a SANDBOX COPY of the build output.
# Refuses to start (exit 3) while the owner is gaming; pauses (60 s polls) if a game starts mid-run. Kills only its own PIDs.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\ui_padspots_shots.ps1 [-Build <bin\Release>] [-Out <dir>] [-Tag after] [-Only <regex>]
# Exit code: 0 all checks pass, 1 a check failed (expected for a build without the fix), 3 refused (owner gaming).
param([string]$Build = "", [string]$Out = "", [string]$Tag = "after", [string]$Only = "", [int]$TimeoutSec = 120)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

function Get-OwnerGameProcs {   # a game / emulator not started by an agent's headless test (those live under ...\wt\ or a Temp folder)
    Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
        Where-Object { -not ($_.Path -and ($_.Path -like '*\SmashClassicPort\wt\*' -or $_.Path -like '*\Temp\*')) }
}
if (@(Get-OwnerGameProcs).Count) { Write-Host "REFUSED: the owner is gaming. Re-run when no game is running."; exit 3 }

if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { throw "no RetroHub.exe in $Build" }
$Build = (Resolve-Path $Build).Path
if (-not $Out) { $Out = Join-Path $root "build\qa_padspots_$Tag" }
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing

# ---- sandbox copy of the build output (never a userdata folder) ----
$sand = Join-Path $root "build\qa_x_padspots_$Tag"
New-Item -ItemType Directory -Force $sand | Out-Null
& robocopy $Build $sand /MIR /XD userdata /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
$exe = Join-Path $sand "RetroHub.exe"
if (-not (Test-Path $exe)) { throw "sandbox copy failed: $exe" }
$userdata = Join-Path $sand "userdata"
$vault = Join-Path $sand "vault"
$script:fail = 0

function Check($name, $ok, $detail) {
    if ($ok) { Write-Host "  PASS $name" } else { Write-Host "  FAIL $name  ($detail)"; $script:fail++ }
}

# Runs one headless hub; returns @{ pad = rows of "pad ..." trace lines; lines = every trace line }.
function Run($name, $pad, $extra, $secs = "4") {
    while (@(Get-OwnerGameProcs).Count) { Write-Host "a game is running: waiting 60 s"; Start-Sleep -Seconds 60 }
    foreach ($d in $userdata, $vault) { if (Test-Path $d) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue } }
    New-Item -ItemType Directory -Force (Join-Path $vault "_account") | Out-Null
    $trace = Join-Path $Out "$name.trace"
    $bmp = Join-Path $Out "$name.bmp"
    Remove-Item $trace, $bmp -ErrorAction SilentlyContinue
    Get-ChildItem $Out -Filter "$name-*.bmp" -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
    Get-ChildItem $Out -Filter "$name-*.png" -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
    $set = @{ RETROHUB_NO_SHELL = "1"; RETROHUB_QA = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1";
              HUB_SCREENSHOT = $bmp; HUB_SHOT_VAULT = $vault; HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = $secs;
              HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_TRACE = $trace }
    if ($pad) { $set["HUB_SHOT_PAD"] = $pad }
    foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
    $psi = New-Object System.Diagnostics.ProcessStartInfo $exe
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WorkingDirectory = $sand
    $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
    foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ($k -match '^(HUB_SHOT_|HUB_START_|HUB_SELECT_|HUB_SCREENSHOT|HUB_SCALE|HUB_UI_SCALE|HUB_TV|HUB_THEME|RETROHUB_)') { $psi.EnvironmentVariables.Remove($k) } }
    foreach ($k in $set.Keys) { $psi.EnvironmentVariables[$k] = [string]$set[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { Write-Host "  timeout: stopping PID $($p.Id)"; try { Stop-Process -Id $p.Id -Force } catch {} }   # our own PID only
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { Write-Host "  hub exit code $($p.ExitCode): $($errTask.Result)" }
    foreach ($b in @(Get-ChildItem $Out -Filter "$name*.bmp" -ErrorAction SilentlyContinue)) {   # BMP -> PNG
        $png = [IO.Path]::ChangeExtension($b.FullName, ".png")
        $img = [System.Drawing.Image]::FromFile($b.FullName); $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
        Remove-Item $b.FullName -ErrorAction SilentlyContinue
    }
    $rows = @(); $lines = @()
    if (Test-Path $trace) {
        $lines = @(Get-Content $trace)
        foreach ($l in $lines) {
            if ($l -match '^pad (\S+) page=(\S+) popup=(\d) friends=(\d) nav=([0-9a-f]+) at=(-?\d+),(-?\d+) .* win=(.*)$') {
                $rows += [pscustomobject]@{ tok = $Matches[1]; popup = [int]$Matches[3]; nav = $Matches[5]; x = [int]$Matches[6]; y = [int]$Matches[7]; win = $Matches[8] }
            }
        }
    }
    return @{ pad = $rows; lines = $lines }
}
function Has($r, $pattern) { return @($r.lines | Where-Object { $_ -match $pattern }).Count -ge 1 }
function Row($r, $tok, $n = 0) { $m = @($r.pad | Where-Object { $_.tok -eq $tok }); if ($m.Count -gt $n) { return $m[$n] } return $null }

$fr = @{ HUB_SHOT_FAKE_FRIENDS = "1" }

# 1. Toast action. In a fake room (3 players), Jake's row in the Friends panel: X = Join -> "Leave your room and join Jake's?" with
#    "Leave and join". A Y made as the toast pops up is not taken (it opens Jake's menu as before; B closes it); a Y once the toast
#    shows the Y glyph presses "Leave and join" (headless: the join itself is skipped with a notice) and opens no menu.
if (-not $Only -or "toast" -match $Only) {
    Write-Host "toast (Leave and join)"
    $extra = $fr + @{ HUB_SHOT_FAKE_ROOM = "1" }
    # View opens the panel; six downs: close / your avatar (Copy before the fix) / add box / Accept / Cancel / Mia / Jake
    $r = Run "toast" "wait,view,down,down,down,down,down,down,shot:row,x,y,shot:early_y,b,wait,wait,shot:toast,y,shot:after_y,wait,shot:after" $extra "5"
    $early = Row $r "y" 0
    Check "toast: the confirmation toast appears (X on Jake's row)" (Has $r '^notice Leave your room and join Jake') "no 'notice Leave your room and join Jake' line"
    Check "toast: a Y pressed as the toast pops up is not taken (Jake's menu opens instead)" ($early -and $early.popup -eq 1) "row=$(if ($early) { "popup=$($early.popup)" } else { 'none' })"
    Check "toast: the Y glyph shows once the toast has settled" (Has $r '^toast-pad-hint Leave and join') "no toast-pad-hint line"
    $late = Row $r "y" 1
    $iFire = [array]::FindIndex([string[]]$r.lines, [Predicate[string]]{ param($s) $s -match '^toast-pad Leave and join' })
    $iEarly = [array]::FindIndex([string[]]$r.lines, [Predicate[string]]{ param($s) $s -match '^pad y ' })
    Check "toast: Y presses Leave and join" ($iFire -ge 0) "no 'toast-pad Leave and join' line"
    Check "toast: only the second Y pressed it" ($iFire -gt $iEarly -and $iEarly -ge 0) "toast-pad at line $iFire, first 'pad y' at line $iEarly"
    Check "toast: the join ran (headless: skipped with a notice)" (Has $r '^notice Headless test run: online skipped') "no 'Headless test run' notice"
    Check "toast: that Y opened no menu" ($late -and $late.popup -eq 0) "row=$(if ($late) { "popup=$($late.popup)" } else { 'none' })"
}

# 2. Your own profile entry (the avatar at the top of the Friends panel): View opens the panel, down reaches the avatar (before the fix:
#    the Copy button), A opens your profile.
if (-not $Only -or "myprof" -match $Only) {
    Write-Host "myprof (your profile entry)"
    $r = Run "myprof" "wait,view,down,shot:focus,a,shot:open" ($fr + @{ HUB_SHOT_PROFILES = "1" }) "5"
    $open = Row $r "a" 0
    Check "myprof: the D-pad focuses your avatar" (Has $r '^myprof-focus') "no myprof-focus line"
    Check "myprof: A opens your profile" ((Has $r '^myprof-open') -and $open -and $open.popup -eq 1 -and $open.win -like 'Profile##frview*') "row=$(if ($open) { "popup=$($open.popup) win=$($open.win)" } else { 'none' })"
}

# 3. Avatar cells in the profile editor, reached by pad only: your avatar -> A (profile) -> down (Edit profile) -> A; the first cell has the
#    focus, right walks the row, A picks the third (Super Nintendo).
if (-not $Only -or "avatar" -match $Only) {
    Write-Host "avatar (profile editor cells, pad only)"
    $r = Run "avatar" "wait,view,down,a,down,a,shot:editor,right,right,shot:focus,a,shot:picked" ($fr + @{ HUB_SHOT_PROFILES = "1" }) "5"
    Check "avatar: the editor opens from the pad" (@($r.pad | Where-Object { $_.win -eq 'Your profile##frprof' }).Count -ge 1) "never in 'Your profile##frprof'"
    Check "avatar: right moves along the row" (Has $r '^prof-avatar-focus c:snes') "no 'prof-avatar-focus c:snes' line"
    Check "avatar: A picks the focused avatar" (Has $r '^prof-avatar c:snes') "no 'prof-avatar c:snes' line"
}
# 3b. The same cells with the editor opened by the mouse (your avatar, then Edit profile), so the base build shows the cells themselves.
if (-not $Only -or "avatarmouse" -match $Only) {
    Write-Host "avatarmouse (editor opened with the mouse, cells walked with the pad)"
    $waits = (@("wait") * 14) -join ","
    $r = Run "avatarmouse" "$waits,shot:editor,right,right,shot:focus,a,shot:picked" ($fr + @{ HUB_SHOT_PROFILES = "1"; HUB_SHOT_FRIENDS = "1"; HUB_SHOT_CLICK = "1018,86;497,180" }) "6"
    Check "avatarmouse: the editor is open" (@($r.pad | Where-Object { $_.win -eq 'Your profile##frprof' }).Count -ge 1) "never in 'Your profile##frprof'"
    Check "avatarmouse: right moves along the cells" (Has $r '^prof-avatar-focus c:snes') "no 'prof-avatar-focus c:snes' line"
    Check "avatarmouse: A picks the focused avatar" (Has $r '^prof-avatar c:snes') "no 'prof-avatar c:snes' line"
}

# 4. Room chat reactions: right from your row in the players list enters the chat (the chat box), up reaches Jake's line, then Mia's;
#    A opens the reactions, right + A picks the second one.
if (-not $Only -or "react" -match $Only) {
    Write-Host "react (room chat reactions)"
    $r = Run "react" "wait,down,right,shot:chatbox,up,shot:line,up,shot:line2,a,shot:popup,right,a,shot:picked" ($fr + @{ HUB_SHOT_PROFILES = "1"; HUB_SHOT_FAKE_ROOM = "1" }) "5"
    Check "react: right from the players list enters the chat box" (Has $r '^chat-enter') "no chat-enter line"
    Check "react: the D-pad focuses a chat line" (Has $r '^rx-focus Mia 1') "no 'rx-focus Mia 1' line"
    Check "react: A opens the reactions" (Has $r '^rx-open Mia 1') "no 'rx-open Mia 1' line"
    Check "react: A on a reaction picks it" (Has $r '^rx-pick ') "no rx-pick line"
}

# 5. Sort chip (Home): up, up, right reaches the chip; X reverses the order (no favourite toggled) and the focus stays on the chip; A still
#    cycles. sortkey: the same with the keyboard's Menu key.
if (-not $Only -or "sort" -match $Only) {
    Write-Host "sort (reverse with X)"
    $r = Run "sort" "wait,up,up,right,shot:focus,x,shot:reversed,a,shot:cycled" @{} "5"
    $f = Row $r "shot:focus" 0; $x1 = Row $r "x" 0
    Check "sort: the D-pad focuses the Sort chip" (Has $r '^sort-focus Sort: Title A-Z') "no 'sort-focus Sort: Title A-Z' line"
    Check "sort: X reverses the order" (Has $r '^sort title rev') "no 'sort title rev' line"
    Check "sort: the focus stays on the chip" ((Has $r '^sort-focus Sort: Title Z-A') -and $f -and $x1 -and $x1.x -eq $f.x -and $x1.y -eq $f.y) "focus at $(if ($x1) { "$($x1.x),$($x1.y)" } else { '-' }) vs $(if ($f) { "$($f.x),$($f.y)" } else { '-' })"
    $lib = Join-Path $vault "library.json"
    Check "sort: X toggled no favourite" (-not ((Test-Path $lib) -and ((Get-Content $lib -Raw) -match '"favorite":true'))) "library.json has a favourite"
    Check "sort: A still cycles the order" (Has $r '^sort last_played') "no 'sort last_played' line"
}
if (-not $Only -or "sortkey" -match $Only) {
    Write-Host "sortkey (reverse with the Menu key)"
    $r = Run "sortkey" "" @{ HUB_SHOT_KEYS = "up,up,right,menu" } "6"
    Check "sortkey: the arrows reach the Sort chip" (Has $r '^sort-focus Sort: Title A-Z') "no sort-focus line"
    Check "sortkey: the Menu key reverses the order" (Has $r '^sort title rev') "no 'sort title rev' line"
}

Write-Host "images and traces in $Out; failures: $($script:fail)"
exit $(if ($script:fail) { 1 } else { 0 })
