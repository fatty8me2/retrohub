# Headless screenshots of the RetroAchievements UI from a MOCKED config + cache (no network, no RA account):
# details modal (About tab) with progress, Home "Recent unlocks", Settings > Account (signed in and signed out).
# usage: qa\cheevos_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\cheevos_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$mock = Join-Path $Out "mock"
Remove-Item -Recurse -Force $mock -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$mock\cache" | Out-Null
$now = [int][double]::Parse((Get-Date -UFormat %s))
$cfgOn = '{"enabled":true,"hardcore":false,"username":"MockPlayer","token":"MOCKTOKEN"}'
$cfgOff = '{"enabled":false,"hardcore":false,"username":"","token":""}'
$game = @{ game_key = "fake:1"; title = "Mock Quest"; game_id = 7; total = 8; unlocked = 3; points_total = 120; points_unlocked = 30; updated = $now
  achievements = @(
    @{ id = 1; title = "First Step"; desc = "Leave the starting town"; badge = "00001"; points = 5; unlock_time = $now - 3600; hardcore = $false },
    @{ id = 2; title = "Treasure Hunter"; desc = "Open 10 chests"; badge = "00002"; points = 10; unlock_time = $now - 7200; hardcore = $true },
    @{ id = 3; title = "Slayer"; desc = "Defeat the first boss"; badge = "00003"; points = 15; unlock_time = $now - 86400; hardcore = $false },
    @{ id = 4; title = "Speed Runner"; desc = "Finish the game in under an hour"; badge = "00004"; points = 25; unlock_time = 0; hardcore = $false },
    @{ id = 5; title = "Completionist"; desc = "Collect everything"; badge = "00005"; points = 50; unlock_time = 0; hardcore = $false },
    @{ id = 6; title = "No Damage"; desc = "Beat level 1 without being hit"; badge = "00006"; points = 5; unlock_time = 0; hardcore = $false },
    @{ id = 7; title = "Secret Room"; desc = "Find the hidden room"; badge = "00007"; points = 5; unlock_time = 0; hardcore = $false },
    @{ id = 8; title = "Last Boss"; desc = "Defeat the final boss"; badge = "00008"; points = 5; unlock_time = 0; hardcore = $false }) }
$recent = @{ recent = @(
  @{ game_key = "fake:1"; game_title = "Mock Quest"; title = "First Step"; desc = "Leave the starting town"; points = 5; time = $now - 3600; hardcore = $false },
  @{ game_key = "fake:4"; game_title = "Pocket Adventure"; title = "Treasure Hunter"; desc = "Open 10 chests"; points = 10; time = $now - 90000; hardcore = $true }) }
[IO.File]::WriteAllText("$mock\on.json", $cfgOn)
[IO.File]::WriteAllText("$mock\off.json", $cfgOff)
[IO.File]::WriteAllText("$mock\cache\game_fake_1.json", ($game | ConvertTo-Json -Depth 5 -Compress))
[IO.File]::WriteAllText("$mock\cache\recent.json", ($recent | ConvertTo-Json -Depth 5 -Compress))

$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_FAKE_LIB = "6"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
$env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
$env:HUB_CHEEVOS_DIR = "$mock\cache"
function Shot($name, $vars) {
  foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
foreach ($sz in "1280x800", "1600x1000") {
  $w = $sz.Split("x")[0]
  Shot "details_$w" @{ HUB_CHEEVOS_CFG = "$mock\on.json"; HUB_SELECT_GAME = "fake:1"; HUB_SHOT_KEYS = "i"; HUB_SHOT_SIZE = $sz; HUB_SHOT_TAB = "0" }
  Shot "home_$w" @{ HUB_CHEEVOS_CFG = "$mock\on.json"; HUB_SHOT_SIZE = $sz }
  Shot "account_in_$w" @{ HUB_CHEEVOS_CFG = "$mock\on.json"; HUB_SHOT_PAGE = "settings:5"; HUB_SHOT_SIZE = $sz }
  Shot "account_out_$w" @{ HUB_CHEEVOS_CFG = "$mock\off.json"; HUB_SHOT_PAGE = "settings:5"; HUB_SHOT_SIZE = $sz }
}
# Tall window (everything below the fold) and the masked password keyboard.
Shot "home_tall" @{ HUB_CHEEVOS_CFG = "$mock\on.json"; HUB_SHOT_SIZE = "1280x2600" }
Shot "account_out_tall" @{ HUB_CHEEVOS_CFG = "$mock\off.json"; HUB_SHOT_PAGE = "settings:5"; HUB_SHOT_SIZE = "1280x2600" }
Shot "account_in_tall" @{ HUB_CHEEVOS_CFG = "$mock\on.json"; HUB_SHOT_PAGE = "settings:5"; HUB_SHOT_SIZE = "1280x2600" }
Shot "osk_password" @{ HUB_SHOT_OSK = "password:hunter2secret" }
