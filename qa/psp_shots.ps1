# Headless screenshots of the PSP library UI at 1280x800: the library with the PSP filter chip + badge, the PSP game details, and the
# boot-art gallery (PSP console + UMD). Fake PSP games come from HUB_SHOT_PSP=1 (lib_state.cpp). No window is shown, everything is muted.
# usage: qa\psp_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\psp_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
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
$base = @{ HUB_SHOT_PSP = "1"; HUB_SHOT_FAKE_LIB = "4"; HUB_SHOT_SIZE = $sz }
Shot "psp_library_all" ($base + @{})
Shot "psp_all_games" ($base + @{ HUB_START_VIEW = "all"; HUB_START_SYS = "psp" })
Shot "psp_details" ($base + @{ HUB_SELECT_GAME = "psp:ulus10041"; HUB_SHOT_KEYS = "i" })
Shot "psp_details_settings" ($base + @{ HUB_SELECT_GAME = "psp:ulus10041"; HUB_SHOT_KEYS = "i"; HUB_SHOT_TAB = "3" })
Shot "psp_bootart" @{ HUB_SHOT_BOOTART = "psp"; HUB_SHOT_SIZE = $sz }
