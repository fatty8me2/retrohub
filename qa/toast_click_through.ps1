# Headless regression check: a mouse click on a toast's action ("Leave and join") belongs to the toast only. Toasts are drawn over the page
# after it, so the click used to reach the page item under the action box too (here: the game page's "Texture filtering" dropdown in the
# Library desk pane opened as the toast acted). Runs a SANDBOX COPY of build\bin\Release (no userdata, scratch vault), muted, SDL dummy
# driver: no window is ever shown, nothing is launched and nothing goes online (fake friends and a fake room). Waits while the owner is
# gaming; a hub that hangs is ended by its PID.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\toast_click_through.ps1 [-Bin dir] [-Out dir]
# Exit 0 = pass, 1 = fail.
param([string]$Bin = '', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_toast_click' }
$sandbox = Join-Path $root 'build\qa_toast_click_bin'
New-Item -ItemType Directory -Force $Out | Out-Null
Wait-NoGame
& robocopy $Bin $sandbox /MIR /XD userdata vault /NFL /NDL /NJH /NJS /NP | Out-Null
$exe = Join-Path $sandbox 'RetroHub.exe'
if (-not (Test-Path $exe)) { throw "no RetroHub.exe in $Bin" }

Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
Remove-Item -Recurse -Force (Join-Path $sandbox 'userdata') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $sandbox 'userdata') | Out-Null
Set-Content -Encoding ascii (Join-Path $sandbox 'userdata\hub.json') '{"show_friend_games": false, "friend_games_v": 1, "setup_done": true, "desk_card_done": true, "username_claimed": true, "net_probe_done": true}'
$vault = Join-Path $sandbox 'vault'
Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
$env:RETROHUB_QA = '1'; $env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$env:HUB_SHOT_VAULT = $vault
$env:HUB_SHOT_LAYOUT = 'desk'
$env:HUB_SHOT_TAB = '3'                    # the pane shows the game's Settings tab: option rows down to the bottom of the window
$env:HUB_SHOT_FAKE_LIB = '14'
$env:HUB_SHOT_FAKE_FRIENDS = '1'
$env:HUB_SHOT_FAKE_ROOM = '1'              # in a room, so Jake's Join asks "Leave and join" in a toast
$env:HUB_SHOT_PAD = 'wait,lb,lb,wait,toast:leave:1.0,wait,shot:toast'   # back to the Library page, then the toast (settled: its action is drawn)
$env:HUB_SHOT_CLICK = '1170,553'           # 1280x720: the action box, which lies over the "Texture filtering" dropdown of the pane
$env:HUB_SHOT_SIZE = '1280x720'
$env:HUB_SHOT_SECS = '6'
$env:HUB_SCREENSHOT = Join-Path $Out 'toast-click.bmp'
$trace = Join-Path $Out 'toast-click.trace'
$env:HUB_SHOT_TRACE = $trace
Remove-Item -Force $trace -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -WorkingDirectory $sandbox -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Host "timed out (ended PID $($p.Id))" }
$p.WaitForExit()
Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }

$lines = if (Test-Path $trace) { @(Get-Content $trace) } else { @() }
$fail = @()
if (-not (@($lines | Where-Object { $_ -eq 'toast-pad-hint Leave and join' }).Count)) { $fail += 'the "Leave and join" toast was never drawn with its action' }
$at = -1
for ($i = 0; $i -lt $lines.Count; ++$i) { if ($lines[$i] -eq 'notice Headless test run: online skipped.') { $at = $i; break } }
if ($at -lt 0) { $fail += 'the click did not press the toast''s action (no "Headless test run: online skipped." notice)' }
else {
    $later = @($lines[$at..($lines.Count - 1)] | Where-Object { $_ -like 'gs-visible *' -or $_ -like 'gs-rows *' -or $_ -like 'gs-set *' })
    if ($later.Count) { $fail += "the click also reached the option row under the toast (its dropdown opened): $($later -join ' | ')" }
}
if ($fail.Count) { $fail | ForEach-Object { Write-Host "FAIL: $_" }; Write-Host "trace: $trace"; exit 1 }
Write-Host 'PASS: a click on a toast action acts on the toast only'
exit 0
