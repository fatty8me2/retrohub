# Pad walks over the game page's Settings tab: stepper rows when the sections flow into two columns (ui_controls.h stepper rule).
# Two columns (1920x1080 full-window page, 2560x1440 desk pane): Left / Right move between the columns and change nothing; A starts
# adjusting a stepper row, Left / Right then step it, A or B ends it, and B never closes the page or leaves the desk pane. One column
# (1280x720): Left / Right step the value as before.
# HEADLESS and muted, from a SANDBOX COPY of build\bin\Release (no userdata), a scratch vault per run; waits while the owner is gaming.
# Reads the HUB_SHOT_TRACE lines: kit-focus (the focused option row and its x), gs-set (a value written), stepper-adjust (on / off-a /
# off-b / off-moved), pad (where the focus is after each press), captured (details=yes: the page is still open at the end).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\stepper_cols.ps1 [-Bin <bin\Release>] [-Out dir]
param([string]$Bin = '', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_stepper_out' }
$box = Join-Path $root 'build\qa_stepper_box'
New-Item -ItemType Directory -Force $Out | Out-Null
Wait-NoGame
& robocopy $Bin $box /MIR /XD userdata vault /NFL /NDL /NJH /NJS /NP | Out-Null
Add-Type -AssemblyName System.Drawing
$hubjson = '{"show_friend_games": false, "friend_games_v": 1, "setup_done": true, "username_claimed": true, "net_probe_done": true, "desk_card_done": true}'
$d6 = 'down,down,down,down,down,down'   # from Play: the tab bar, the Graphics heading, Fullscreen, Internal resolution, Aspect ratio, Anti-aliasing (MSAA)
$runs = @(
    @{ n = 'trad2-cross';  size = '1920x1080'; layout = 'traditional'; pad = "$d6,shot:msaa,right,shot:right,left,shot:left" },
    @{ n = 'trad2-adjust'; size = '1920x1080'; layout = 'traditional'; pad = "$d6,a,shot:adjusting,right,right,left,a,shot:done,right,shot:cross" },
    @{ n = 'trad2-b';      size = '1920x1080'; layout = 'traditional'; pad = "$d6,a,right,b,shot:afterb,right,shot:cross" },
    @{ n = 'desk2-b';      size = '2560x1440'; layout = 'desk';        pad = "$d6,a,shot:adjusting,right,b,shot:afterb,right,shot:cross" },
    @{ n = 'trad1-step';   size = '1280x720';  layout = 'traditional'; pad = "$d6,right,shot:stepped" }
)
function Run([hashtable]$r) {
    Wait-NoGame
    foreach ($d in 'userdata', 'vault') { Remove-Item -Recurse -Force (Join-Path $box $d) -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force (Join-Path $box 'userdata'), (Join-Path $box 'vault\_account') | Out-Null
    Set-Content -Encoding ascii (Join-Path $box 'userdata\hub.json') $hubjson
    $trace = Join-Path $Out "$($r.n).trace"
    Remove-Item $trace -ErrorAction SilentlyContinue
    Get-ChildItem $Out -Filter "$($r.n)*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $box 'RetroHub.exe'; $psi.WorkingDirectory = $box; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    foreach ($kv in @("HUB_SCREENSHOT=$Out\$($r.n).bmp", "HUB_SHOT_SIZE=$($r.size)", 'HUB_SHOT_SECS=6', "HUB_SHOT_VAULT=$box\vault", "HUB_SHOT_TRACE=$trace",
                      "HUB_SHOT_LAYOUT=$($r.layout)", 'HUB_SHOT_FAKE_LIB=all', 'HUB_SHOT_DETAILS=fake:n64-plain', 'HUB_SHOT_TAB=3', "HUB_SHOT_PAD=$($r.pad)",
                      'RETROHUB_QA=1', 'SSB64_MUTE=1', 'RETRORUN_MUTE=1', 'N64RUN_MUTE=1', 'RETROHUB_NO_CLOUD=1')) {
        if ($kv -match '^([^=]+)=(.*)$') { $psi.EnvironmentVariables[$Matches[1]] = $Matches[2] }
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; return @{ code = -1; lines = @(); gs = '' } }   # only the PID started here
    foreach ($b in Get-ChildItem $Out -Filter "$($r.n)*.bmp") { $img = [System.Drawing.Image]::FromFile($b.FullName); $img.Save([IO.Path]::ChangeExtension($b.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose(); Remove-Item $b.FullName }
    $gsf = Join-Path $box 'vault\_settings\game_settings.json'
    return @{ code = $p.ExitCode; lines = @(if (Test-Path $trace) { Get-Content $trace }); gs = $(if (Test-Path $gsf) { (Get-Content $gsf -Raw) -replace '\s+', ' ' } else { '' }) }
}
function PadAt($lines, [string]$tok, [int]$nth) {   # x of the focus after the nth press of $tok (pad lines are written once a press has settled)
    $k = 0
    foreach ($l in $lines) { if ($l -match "^pad $tok page=\S+ popup=\S+ friends=\S+ nav=\S+ at=(-?\d+),(-?\d+) .* win=(.*)$") { $k++; if ($k -eq $nth) { return @{ x = [int]$Matches[1]; y = [int]$Matches[2]; win = $Matches[3] } } } }
    return $null
}
$fail = 0
foreach ($r in $runs) {
    $res = Run $r
    $L = $res.lines
    $sets = @($L | Where-Object { $_ -like 'gs-set *' })
    $adj = @($L | Where-Object { $_ -like 'stepper-adjust *' })
    $cap = $L | Where-Object { $_ -like 'captured *' } | Select-Object -Last 1
    $msaa = @($L | Where-Object { $_ -like 'kit-focus Anti-aliasing (MSAA)*' }).Count -gt 0
    $w = [int]($r.size -split 'x')[0]
    $bad = @()
    if ($res.code -ne 0) { $bad += "exit $($res.code)" }
    if (-not $msaa) { $bad += 'never reached the Anti-aliasing (MSAA) row' }
    switch ($r.n) {
        'trad2-cross' {
            $rt = PadAt $L 'right' 1; $lt = PadAt $L 'left' 1
            if ($sets.Count) { $bad += "a value changed: $($sets -join '; ')" }
            if (-not $rt -or $rt.x -lt $w / 2) { $bad += "Right did not reach the second column (x=$($rt.x))" }
            if (-not $lt -or $lt.x -ge $w / 2) { $bad += "Left did not come back to the first column (x=$($lt.x))" }
            if ($adj.Count) { $bad += "adjusting started: $($adj -join '; ')" }
        }
        'trad2-adjust' {
            if (($adj -join '|') -notmatch 'on Anti-aliasing' -or ($adj -join '|') -notmatch 'off-a Anti-aliasing') { $bad += "adjust on / off-a missing: $($adj -join '; ')" }
            if ($sets.Count -ne 3) { $bad += "expected 3 steps while adjusting, got $($sets.Count): $($sets -join '; ')" }
            if ($res.gs -notmatch '"n64\.aa"\s*:\s*"2"') { $bad += "game_settings.json does not hold n64.aa=2: $($res.gs)" }
            $rt = PadAt $L 'right' 3
            if (-not $rt -or $rt.x -lt $w / 2) { $bad += "Right after A (done) did not reach the second column (x=$($rt.x))" }
        }
        { $_ -in 'trad2-b', 'desk2-b' } {
            if (($adj -join '|') -notmatch 'off-b Anti-aliasing') { $bad += "B did not end the adjusting: $($adj -join '; ')" }
            if ($sets.Count -ne 1) { $bad += "expected 1 step, got $($sets.Count): $($sets -join '; ')" }
            $bp = PadAt $L 'b' 1; $rt = PadAt $L 'right' 2
            if ($r.n -eq 'trad2-b' -and (-not $bp -or $bp.win -notlike 'details##uisheet*')) { $bad += "after B the focus left the page (win=$($bp.win))" }
            if ($r.n -eq 'desk2-b' -and (-not $bp -or $bp.win -notlike '*##desk_pane*')) { $bad += "after B the focus left the desk pane (win=$($bp.win))" }
            if ($r.n -eq 'desk2-b' -and @($L | Where-Object { $_ -like 'desk-focus list*' }).Count) { $bad += 'B went back to the list' }
            if (-not $rt -or $rt.x -lt $w / 2) { $bad += "Right after B did not reach the second column (x=$($rt.x))" }
            if ($cap -notmatch 'details=yes') { $bad += "the page was closed: $cap" }
        }
        'trad1-step' {
            if ($sets.Count -ne 1 -or $sets[0] -notmatch 'n64\.aa=2') { $bad += "one column: Right should step the value once (got $($sets -join '; '))" }
            if ($adj.Count) { $bad += "one column must not use the adjusting state: $($adj -join '; ')" }
        }
    }
    if ($bad.Count) { $fail++; Write-Output "$($r.n): FAIL: $($bad -join '; ')" } else { Write-Output "$($r.n): ok  (steps: $($sets.Count), adjust: $($adj -join ', '))" }
}
Remove-Item -Recurse -Force $box -ErrorAction SilentlyContinue
Write-Output "PNGs and traces in $Out; $fail failure(s)"
exit ([int]($fail -gt 0))
