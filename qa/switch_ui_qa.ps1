# Headless screenshots for the Switch / Switch 2 UI pass not covered by switch_shots.ps1: the keys + firmware prompt (switchkeys),
# the Settings > Video & Games emulator list and Game defaults combo (Switch = index 12, SNES = index 11), and the Cases view.
# Fake Switch games come from HUB_SHOT_SWITCH=1 (lib_state.cpp). No window is shown, every launch is muted, each shot is its own PID.
# usage: qa\switch_ui_qa.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\switch_ui_qa", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
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
# 1. keys + firmware prompt
Shot "switch_keys_modal" ($base + @{ HUB_SHOT_EMU = "switchkeys" })
# 2. Switch 2 game details (Mario Kart World, the nx2 entry) with its settings tab
Shot "switch2_details" ($base + @{ HUB_SELECT_GAME = "switch:0400000000a1b000"; HUB_SHOT_KEYS = "i" })
Shot "switch2_details_settings" ($base + @{ HUB_SELECT_GAME = "switch:0400000000a1b000"; HUB_SHOT_KEYS = "i"; HUB_SHOT_TAB = "3" })
# 3-5. Settings > Video & Games: top, the Playing emulator list (Lichii line), and the Game defaults combo for Switch and SNES
Shot "settings_video_top" ($base + @{ HUB_SHOT_PAGE = "settings:2" })
Shot "settings_video_emulators" ($base + @{ HUB_SHOT_PAGE = "settings:2"; HUB_SHOT_WHEEL = "640,400,8" })
Shot "settings_gamedefs_switch" ($base + @{ HUB_SHOT_PAGE = "settings:2"; HUB_SHOT_WHEEL = "640,400,40"; HUB_SHOT_GS_SYS = "12" })
Shot "settings_gamedefs_snes" ($base + @{ HUB_SHOT_PAGE = "settings:2"; HUB_SHOT_WHEEL = "640,400,40"; HUB_SHOT_GS_SYS = "11" })
# 6. Cases view: browse, and the Switch cartridge filter
Shot "cases_browse" ($base + @{ HUB_SHOT_CASES = "browse" })
Shot "cases_switch" ($base + @{ HUB_SHOT_CASES = "cartridge:switch" })
