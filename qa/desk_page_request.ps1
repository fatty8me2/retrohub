# Headless regression check: a game's page asked for (here: the command palette) while the Library desk's search hides that game.
# The desk must clear the search, open the game's (closed) console group and put the list cursor on the game; it used to rebuild its
# list from the old search's matches, so the cursor stayed on a search match and the game's group stayed closed (the pane showed the
# game, the list another one). Runs a SANDBOX COPY of build\bin\Release (no userdata, scratch vault), muted, SDL dummy driver: no window
# is ever shown and nothing is launched. Waits while the owner is gaming; a hub that hangs is ended by its PID.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\desk_page_request.ps1 [-Bin dir] [-Out dir]
# Exit 0 = pass, 1 = fail.
param([string]$Bin = '', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_desk_page_request' }
$sandbox = Join-Path $root 'build\qa_desk_page_request_bin'
New-Item -ItemType Directory -Force $Out | Out-Null
Wait-NoGame
& robocopy $Bin $sandbox /MIR /XD userdata vault /NFL /NDL /NJH /NJS /NP | Out-Null
$exe = Join-Path $sandbox 'RetroHub.exe'
if (-not (Test-Path $exe)) { throw "no RetroHub.exe in $Bin" }

Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
Remove-Item -Recurse -Force (Join-Path $sandbox 'userdata') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $sandbox 'userdata') | Out-Null
Set-Content -Encoding ascii (Join-Path $sandbox 'userdata\hub.json') '{"show_friend_games": false, "friend_games_v": 1}'
$vault = Join-Path $sandbox 'vault'
Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
$env:RETROHUB_QA = '1'; $env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$env:HUB_SHOT_VAULT = $vault
$env:HUB_SHOT_LAYOUT = 'desk'
$env:HUB_SHOT_FAKE_LIB = '60'                 # more than 40 games: only Recent and the first console group start open (GameCube is closed)
$env:HUB_SHOT_SEARCH = 'Test Game 1'          # hides "Test Game 23" (fake:22, GameCube)
$env:HUB_SHOT_PALETTE = 'test game 23'        # the palette opens with that text; Down + A picks the game: its page is asked for
$env:HUB_SHOT_PAD = 'wait,wait,wait,wait,wait,wait,down,a,wait,wait,shot:after'
$env:HUB_SHOT_SIZE = '1280x720'
$env:HUB_SHOT_SECS = '6'
$env:HUB_SCREENSHOT = Join-Path $Out 'desk-page-request.bmp'
$trace = Join-Path $Out 'desk-page-request.trace'
$env:HUB_SHOT_TRACE = $trace
Remove-Item -Force $trace -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -WorkingDirectory $sandbox -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Host "timed out (ended PID $($p.Id))" }
$p.WaitForExit()
Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }

$lines = if (Test-Path $trace) { @(Get-Content $trace) } else { @() }
$at = -1
for ($i = 0; $i -lt $lines.Count; ++$i) { if ($lines[$i] -eq 'desk-page fake:22') { $at = $i; break } }
$fail = @()
if ($at -lt 0) { $fail += 'the page request never reached the desk (no "desk-page fake:22")' }
else {
    $after = $lines[$at..($lines.Count - 1)]
    $cursor = @($lines[0..$at] + $after | Where-Object { $_ -like 'desk-cursor *' }) | Select-Object -Last 1
    if (-not ($cursor -match 'key=fake:22 ')) { $fail += "the list cursor is not on the game: $cursor" }
    if (-not (@($lines | Where-Object { $_ -eq 'desk-group gc open' }).Count)) { $fail += 'its GameCube group was not opened' }
    $model = @($after | Where-Object { $_ -like 'desk-model *' }) | Select-Object -Last 1
    if (-not ($model -match 'matches=62 ')) { $fail += "the list does not show the whole library after the search was cleared: $model" }
}
if ($fail.Count) { $fail | ForEach-Object { Write-Host "FAIL: $_" }; Write-Host "trace: $trace"; exit 1 }
Write-Host 'PASS: desk page request clears the search, opens the group and puts the cursor on the game'
exit 0
