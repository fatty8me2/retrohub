# Headless screenshots of the Switch library UI at 1280x800: the library with the Switch filter chip + badge, the Switch game details, and the
# boot-art gallery (Switch console + game card). Fake Switch games come from HUB_SHOT_SWITCH=1 (lib_state.cpp). No window is shown, everything is muted.
# usage: qa\switch_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\switch_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
$env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
function Shot($name, $vars) {
  foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
$sz = "1280x800"
$base = @{ HUB_SHOT_SWITCH = "1"; HUB_SHOT_FAKE_LIB = "4"; HUB_SHOT_SIZE = $sz }
Shot "switch_library_all" ($base + @{})
Shot "switch_all_games" ($base + @{ HUB_START_VIEW = "all"; HUB_START_SYS = "switch" })
Shot "switch_details" ($base + @{ HUB_SELECT_GAME = "switch:0100000000010000"; HUB_SHOT_KEYS = "i" })
Shot "switch_details_settings" ($base + @{ HUB_SELECT_GAME = "switch:0100000000010000"; HUB_SHOT_KEYS = "i"; HUB_SHOT_TAB = "3" })
Shot "switch_bootart" @{ HUB_SHOT_BOOTART = "switch"; HUB_SHOT_SIZE = $sz }
Shot "switch2_bootart" @{ HUB_SHOT_BOOTART = "switch2"; HUB_SHOT_SIZE = $sz }
Shot "all_bootart" @{ HUB_SHOT_BOOTART = "all"; HUB_SHOT_SIZE = "1600x800" }
foreach ($t in @(0.5, 1.5, 2.6, 3.6)) {
  Shot "switch_bootanim_$t" ($base + @{ HUB_SHOT_BOOTANIM = "switch:$t" })
}
Shot "switch2_bootanim" ($base + @{ HUB_SHOT_BOOTANIM = "switch:3.2"; HUB_SELECT_GAME = "switch:0400000000a1b000" })
