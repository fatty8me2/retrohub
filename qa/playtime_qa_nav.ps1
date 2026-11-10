# QA lane "nav": play time must not break keyboard / pad navigation, and "Most played" must be reachable by RT and by the Cases picker.
# Headless only (SDL dummy driver, no window), muted, fake library (fake:0..7). Never touches the owner's install or vault.
# usage: qa\playtime_qa_nav.ps1 [-Out <dir>] [-Exe <RetroHub.exe>]
# Output: <Out>\<name>.png per screenshot, <Out>\<name>.trace.txt per run, <Out>\results.txt with the PASS/FAIL lines. Exit code = failures.
param([string]$Out = "$env:TEMP\playtime_qa_nav",
      [string]$Exe = (Join-Path $PSScriptRoot "..\build\bin\Release\RetroHub.exe"))
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$hubjson = Join-Path $root "build\bin\Release\userdata\hub.json"   # worktree-local settings: every run starts fresh (default sort = Title)
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
$base = @{ RETROHUB_NO_SHELL = '1'; SSB64_MUTE = '1'; RETRORUN_MUTE = '1'; N64RUN_MUTE = '1'; RETROHUB_QA = '1'; HUB_SHOT_NOWAIT = '1';
           HUB_SHOT_SECS = '4'; HUB_SHOT_SIZE = '1280x800'; HUB_SHOT_FAKE_LIB = '8'; HUB_SHOT_PLAYTIME = '1' }
$script:fail = 0
$script:log = @()
function Say([string]$s) { $script:log += $s; Write-Host $s }
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Say "PASS $name" } else { Say "FAIL $name  ($detail)"; $script:fail++ }
}

# One headless run: base env + extra vars, screenshot -> <name>.png, trace -> <name>.trace.txt. Killed by PID on overrun.
function Run([string]$name, [hashtable]$extra) {
    $set = @{}
    foreach ($k in $base.Keys) { $set[$k] = $base[$k] }
    foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
    $bmp = Join-Path $Out "$name.bmp"; $png = Join-Path $Out "$name.png"; $trace = Join-Path $Out "$name.trace.txt"
    $set['HUB_SCREENSHOT'] = $bmp; $set['HUB_SHOT_TRACE'] = $trace
    Remove-Item -LiteralPath $bmp, $trace, $png -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $hubjson -ErrorAction SilentlyContinue
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Say "TIMEOUT $name (killed our pid $($p.Id))" }
    foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $bmp) {
        $img = [System.Drawing.Image]::FromFile($bmp)
        $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
        Remove-Item -LiteralPath $bmp
    } else { Say "NO BMP for $name" }
    $lines = @()
    if (Test-Path -LiteralPath $trace) { $lines = @(Get-Content -LiteralPath $trace) }
    return @{ png = $png; lines = $lines }
}

# Pad trace lines: "pad <tok> page=.. popup=.. friends=.. nav=<hex> at=x,y osk=.. wiz=.. name=[..] oskt=[..] sy=.. win=<name>"
function Pads($lines) {
    $rows = @()
    foreach ($l in $lines) {
        if ($l -match '^pad (\S+) page=(\S+) popup=(\d) friends=(\d) nav=([0-9a-f]+) at=(-?\d+),(-?\d+) .* win=(.*)$') {
            $rows += [pscustomobject]@{ tok = $Matches[1]; page = $Matches[2]; popup = [int]$Matches[3]; nav = $Matches[5]; x = [int]$Matches[6]; y = [int]$Matches[7]; win = $Matches[8] }
        }
    }
    return , $rows
}
function Show($rows) { ($rows | ForEach-Object { "$($_.tok)[popup=$($_.popup) nav=$($_.nav) at=$($_.x),$($_.y) win=$($_.win)]" }) -join '  ' }

# ---------------- Check 1: game page focus (keys and pad) ----------------
Say "== 1. game page focus"
$page = @{ HUB_SELECT_GAME = 'fake:0'; HUB_SHOT_PAD = '1' }
$null = Run "1_page_open" ($page + @{ HUB_SHOT_KEYS = 'i' })              # focus ring should sit on Play
$null = Run "1_page_down1" ($page + @{ HUB_SHOT_KEYS = 'i,down' })        # one step through the buttons, not onto the strip
$null = Run "1_page_down2" ($page + @{ HUB_SHOT_KEYS = 'i,down,down' })
$null = Run "1_page_esc" ($page + @{ HUB_SHOT_KEYS = 'i,esc' })           # page must close (library visible, no details)
# Pad-driven trace of the same page: y opens details, then d-pad moves, b closes. One trace line per press.
$t1 = Run "1_page_pad_trace" @{ HUB_SELECT_GAME = 'fake:0'; HUB_SHOT_PAD = 'y,down,down,up,down,b' }
$p1 = Pads $t1.lines
Say ("   trace: " + (Show $p1))
Check "1a pad Y opens the game page" ($p1.Count -ge 1 -and $p1[0].popup -eq 1) ("rows=$($p1.Count)")
$moves1 = @($p1 | Select-Object -Skip 1 | Select-Object -First 4)
Check "1b d-pad moves never drop focus (nav 00000000)" (@($moves1 | Where-Object { $_.nav -eq '00000000' }).Count -eq 0 -and $moves1.Count -eq 4) (Show $moves1)
Check "1c d-pad moves across several targets" ((@($moves1 | Select-Object -ExpandProperty nav -Unique)).Count -ge 2) (Show $moves1)
Check "1d B closes the game page" ($p1.Count -ge 6 -and $p1[-1].popup -eq 0) (Show $p1)

# ---------------- Check 2: RT cycles the sort outside All Games ----------------
Say "== 2. RT cycles the sort"
$rtFound = 0
foreach ($n in 1..6) {
    $pad = ((1..$n | ForEach-Object { 'rt' }) -join ',')
    $r = Run "2_rt_$n" @{ HUB_SHOT_PAD = $pad; HUB_SHOT_SECS = '8' }
    $sorts = @($r.lines | Where-Object { $_ -like 'sort *' })
    Say "   rt x$n -> " + ($(if ($sorts.Count) { $sorts -join ' | ' } else { '(no sort line)' }))
    if (-not $rtFound -and (@($sorts | Where-Object { $_ -eq 'sort most_played' }).Count -gt 0)) { $rtFound = $n }
}
Check "2a RT reaches 'sort most_played' (press count)" ($rtFound -gt 0) "never reached in 1..6 presses"
if ($rtFound -gt 0) { Say "   most_played reached after $rtFound RT press(es); screenshot: $(Join-Path $Out "2_rt_$rtFound.png")" }

# ---------------- Check 3: Cases sort picker ----------------
Say "== 3. Cases sort picker"
$null = Run "3_cases_picker_open" @{ HUB_START_VIEW = 'all'; HUB_SHOT_LIBVIEW = 'cases'; HUB_SHOT_KEYS = 'o'; HUB_SHOT_SECS = '6' }
$r3 = Run "3_cases_most_played" @{ HUB_START_VIEW = 'all'; HUB_SHOT_LIBVIEW = 'cases'; HUB_SHOT_KEYS = 'o,down,down,enter'; HUB_SHOT_SECS = '8' }
$s3 = @($r3.lines | Where-Object { $_ -like 'sort *' -or $_ -like 'cases-nav *' })
Say ("   trace: " + ($s3 -join ' | '))
Check "3a picker opens with O" (@($r3.lines | Where-Object { $_ -eq 'cases-nav sort-open' }).Count -ge 1) ($r3.lines -join '|')
Check "3b picking 'Most played' (2 downs + Enter) traces sort most_played" (@($r3.lines | Where-Object { $_ -eq 'sort most_played' }).Count -ge 1) ($s3 -join '|')

# ---------------- Check 4: All Games grid keeps moving with hours subtitles ----------------
Say "== 4. All Games grid navigation (most played)"
$grid = @{ HUB_START_VIEW = 'all'; HUB_SHOT_SORT = 'most_played'; HUB_SHOT_LIBVIEW = 'grid'; HUB_SHOT_SECS = '6' }
$null = Run "4_grid_baseline" $grid
$null = Run "4_grid_keys" ($grid + @{ HUB_SHOT_KEYS = 'right,right,down' })
$t4 = Run "4_grid_pad_trace" ($grid + @{ HUB_SHOT_PAD = 'right,right,down' })
$p4 = Pads $t4.lines
Say ("   trace: " + (Show $p4))
Check "4a three d-pad moves on the grid, each one traced" ($p4.Count -eq 3) "rows=$($p4.Count)"
Check "4b every move lands on a tile (nav not lost)" (@($p4 | Where-Object { $_.nav -eq '00000000' }).Count -eq 0 -and $p4.Count -eq 3) (Show $p4)
Check "4c consecutive moves change focus" ($p4.Count -eq 3 -and $p4[0].nav -ne $p4[1].nav -and $p4[1].nav -ne $p4[2].nav) (Show $p4)

# Results file (plain text, next to the PNGs)
$script:log | Set-Content -LiteralPath (Join-Path $Out 'results.txt') -Encoding utf8
Say "results in $Out ; failures: $($script:fail)"
exit $script:fail
