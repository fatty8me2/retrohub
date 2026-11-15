# Headless gamepad walk onto controls a controller could not reach before (ui-padreach lane):
#   1. a save-state cell on a game's Saves tab, 2. a screenshot thumbnail on the Screenshots tab (the strip must scroll to it),
#   3. a theme swatch in Settings > General, 4. B closes the "Plan a session" (game night) dialog.
# Each case walks the D-pad onto the control, saves shots (<case>-<name>.png) and checks the HUB_SHOT_TRACE lines.
# No visible window, muted, no network, scratch vault only. Runs from a SANDBOX COPY of the build output (no userdata).
# Refuses to start (exit 3) while the owner is gaming; pauses (60 s polls) if a game starts mid-run. Kills only its own PIDs.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\ui_padreach_shots.ps1 [-Build <bin\Release>] [-Out <dir>] [-Tag after] [-Only <regex>]
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
if (-not $Out) { $Out = Join-Path $root "build\qa_padreach_$Tag" }
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing

# ---- sandbox copy of the build output (never a userdata folder) ----
$sand = Join-Path $root "build\qa_x_padreach_$Tag"
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
    $set = @{ RETROHUB_NO_SHELL = "1"; RETROHUB_QA = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1";
              HUB_SCREENSHOT = $bmp; HUB_SHOT_VAULT = $vault; HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = $secs;
              HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAD = $pad; HUB_SHOT_TRACE = $trace }
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

# 1. Save-state cells (Saves tab of fake:1 with fake states): down from Play into the cells, right to the next one, A selects it.
if (-not $Only -or "states" -match $Only) {
    Write-Host "states (Saves tab cells)"
    # Resume -> Saves tab -> first control of the tab -> the Auto cell; right = Slot 0; A selects it; down = Slot 6 (focus only, Slot 0
    # stays selected); down = "Resume from this" for Slot 0 (look at states-resume.png)
    $r = Run "states" "wait,down,down,down,shot:cell,right,shot:cell_right,a,shot:after_a,down,shot:below,down,shot:resume" @{ HUB_SHOT_STATES = "1"; HUB_SHOT_DETAILS = "fake:1"; HUB_SHOT_TAB = "1" } "5"
    Check "states: the D-pad focuses the Auto cell" (Has $r '^state-focus fake:1 slot=-1') "no 'state-focus fake:1 slot=-1' trace line"
    Check "states: right moves to Slot 0" (Has $r '^state-focus fake:1 slot=0') "no 'state-focus fake:1 slot=0' trace line"
    Check "states: A on Slot 0 selects it" (Has $r '^state-pick fake:1 slot=0') "no 'state-pick fake:1 slot=0' trace line"
}

# 2. Screenshot thumbnails (Screenshots tab of fake:0 with six fake pictures): focus walks right along the strip, which scrolls; A opens the viewer.
if (-not $Only -or "shots" -match $Only) {
    Write-Host "shots (Screenshots tab thumbnails)"
    # Play -> Screenshots tab -> Open folder -> first picture; right x5 = the sixth (off screen until the strip scrolls); A = viewer
    $r = Run "shots" "wait,down,down,down,shot:thumb_first,right,right,right,right,right,shot:thumb_last,a,shot:viewer" @{ HUB_SHOT_GALLERY = "1"; HUB_SHOT_DETAILS = "fake:0"; HUB_SHOT_TAB = "2" } "5"
    Check "shots: the first picture gets the focus" (Has $r '^shots-focus i=0 ') "no 'shots-focus i=0' line"
    Check "shots: the last thumbnail is focused and fully visible (strip scrolled)" (Has $r '^shots-focus i=5 .*visible=1') "no 'shots-focus i=5 ... visible=1' line"
    Check "shots: A opens the viewer" (Has $r '^shots-open i=5') "no shots-open i=5 line"
}

# 3. Theme swatches (Settings > General): walk down to the theme row, right along it, A picks the focused theme.
if (-not $Only -or "theme" -match $Only) {
    Write-Host "theme (Settings swatches)"
    # seven downs from the top of Settings > General reach the theme row (Midnight); right x2 = Light (previewed); A picks it
    $r = Run "theme" "wait,down,down,down,down,down,down,down,shot:swatch,right,right,shot:swatch_right,a,shot:after_a,down,shot:below" @{ HUB_SHOT_PAGE = "settings:general" } "5"
    Check "theme: the D-pad focuses the first swatch" (Has $r '^theme-focus preset=0') "no 'theme-focus preset=0' trace line"
    Check "theme: right moves along the row" (Has $r '^theme-focus preset=2') "no 'theme-focus preset=2' trace line"
    Check "theme: A picks the focused theme" (Has $r '^theme-pick preset=2') "no 'theme-pick preset=2' trace line"
}

# 4. Game night "Plan a session": opened from Mia's chat header, B closes it.
if (-not $Only -or "gamenight" -match $Only) {
    Write-Host "gamenight (B closes the dialog)"
    # a mouse click on the chat header's "Plan a session" pill (1280x800 layout) opens it while the pad waits; then A opens the Game
    # list, the first B only closes that list, the second B closes the dialog and the chat stays open
    $extra = @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FRIENDS = "1"; HUB_SHOT_CHAT = "1"; HUB_SHOT_CLICK = "1153,40" }
    $r = Run "gamenight" "wait,wait,wait,wait,wait,wait,wait,wait,wait,wait,wait,wait,shot:open,a,shot:list,b,shot:list_closed,b,shot:closed" $extra "5"
    $open = @($r.pad | Where-Object { $_.tok -eq "shot:open" })
    Check "gamenight: the dialog opens" ($open.Count -ge 1 -and $open[-1].popup -eq 1 -and $open[-1].win -eq "##plan_night") "row=$(if ($open.Count) { "popup=$($open[-1].popup) win=$($open[-1].win)" } else { 'none' })"
    $b = @($r.pad | Where-Object { $_.tok -eq "b" })
    Check "gamenight: the first B only closes the Game list" ($b.Count -ge 1 -and $b[0].popup -eq 1 -and $b[0].win -eq "##plan_night") "row=$(if ($b.Count) { "popup=$($b[0].popup) win=$($b[0].win)" } else { 'none' })"
    Check "gamenight: the second B closes the dialog" ($b.Count -ge 2 -and $b[-1].popup -eq 0) "popup after the second B=$(if ($b.Count -ge 2) { $b[-1].popup } else { 'no row' })"
    Check "gamenight: the chat stays open with focus on its header" ($b.Count -ge 2 -and $b[-1].win -eq "##friends" -and $b[-1].nav -ne "00000000") "row=$(if ($b.Count -ge 2) { "win=$($b[-1].win) nav=$($b[-1].nav)" } else { 'none' })"
}

Write-Host "images and traces in $Out; failures: $($script:fail)"
exit $(if ($script:fail) { 1 } else { 0 })
