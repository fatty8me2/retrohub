# Headless regression check: the "Send to a friend" picker (screenshot viewer) by controller. A on a friend ticks it, A again unticks it.
# The row's ImGui ID used to be its whole label, so ticking changed it: the pad focus was left on an item that no longer existed and the
# second A did nothing. Runs a SANDBOX COPY of build\bin\Release (no userdata, scratch vault), muted, SDL dummy driver: no window is ever
# shown, nothing is sent (fake friends, no network). Waits while the owner is gaming; a hub that hangs is ended by its PID.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\friend_pick_pad.ps1 [-Bin dir] [-Out dir]
# Exit 0 = pass, 1 = fail.
param([string]$Bin = '', [string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_friend_pick' }
$sandbox = Join-Path $root 'build\qa_friend_pick_bin'
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
$env:HUB_SHOT_FAKE_LIB = '14'
$env:HUB_SHOT_FAKE_FRIENDS = '1'
$env:HUB_SHOT_GALLERY = '1'          # made-up screenshots for the game ...
$env:HUB_SHOT_DETAILS = 'fake:0'     # ... whose page opens on its Screenshots tab,
$env:HUB_SHOT_TAB = '2'
$env:HUB_SHOT_VIEWER = '1'           # the viewer opens on a picture,
$env:HUB_SHOT_FRIEND_PICK = '1'      # and its "Send to a friend" picker over it (the pad focus on the first friend)
$env:HUB_SHOT_PAD = 'wait,wait,wait,wait,wait,wait,a,a,shot:after'
$env:HUB_SHOT_SIZE = '1280x720'
$env:HUB_SHOT_SECS = '5'
$env:HUB_SCREENSHOT = Join-Path $Out 'friend-pick.bmp'
$trace = Join-Path $Out 'friend-pick.trace'
$env:HUB_SHOT_TRACE = $trace
Remove-Item -Force $trace -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -WorkingDirectory $sandbox -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Host "timed out (ended PID $($p.Id))" }
$p.WaitForExit()
Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }

$lines = if (Test-Path $trace) { @(Get-Content $trace) } else { @() }
$toggles = @($lines | Where-Object { $_ -like 'fpick-toggle *' })
$fail = @()
if ($toggles.Count -ne 2) { $fail += "expected two toggles (on, then off), got $($toggles.Count): $($toggles -join ' | ')" }
elseif (-not ($toggles[0] -like '* on' -and $toggles[1] -like '* off' -and ($toggles[0] -replace ' on$', '') -eq ($toggles[1] -replace ' off$', ''))) {
    $fail += "the second A did not untick the same friend: $($toggles -join ' | ')"
}
if ($fail.Count) { $fail | ForEach-Object { Write-Host "FAIL: $_" }; Write-Host "trace: $trace"; exit 1 }
Write-Host 'PASS: A ticks a friend in the picker and A again unticks it'
exit 0
