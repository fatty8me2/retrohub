# Headless screenshots of the in-game quick menu (main / save / load) and the screenshots gallery in the details modal.
# No window is shown (SDL dummy video driver + software renderer). usage: qa\quickmenu_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\qm_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_FAKE_LIB = "3"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
function Shot($name, $vars) {
  foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
Shot "qm_main_1280"  @{ HUB_SHOT_QM = "main"; HUB_SHOT_SIZE = "1280x800" }
Shot "qm_save_1280"  @{ HUB_SHOT_QM = "save"; HUB_SHOT_SIZE = "1280x800" }
Shot "qm_load_1600"  @{ HUB_SHOT_QM = "load"; HUB_SHOT_SIZE = "1600x1000" }
Shot "qm_main_1600"  @{ HUB_SHOT_QM = "main"; HUB_SHOT_SIZE = "1600x1000" }
Shot "gallery_1280"  @{ HUB_SHOT_GALLERY = "1"; HUB_START_PAGE = "0"; HUB_SELECT_GAME = "fake:0"; HUB_SHOT_KEYS = "i"; HUB_SHOT_SIZE = "1280x800" }
# speed / volume / mute rows: offline, online (speed disabled), an external emulator (no speed, screenshot entry), and pad nav (down, right, right)
Shot "qm_offline_1280" @{ HUB_SHOT_QM = "main"; HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_QM_VOL = "65" }
Shot "qm_online_1280"  @{ HUB_SHOT_QM = "main"; HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_QM_ONLINE = "1"; HUB_SHOT_QM_VOL = "35"; HUB_SHOT_QM_MUTE = "1" }
Shot "qm_ext_1280"     @{ HUB_SHOT_QM = "main"; HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_QM_EXT = "1"; HUB_SHOT_QM_VOL = "80" }
Shot "qm_speednav_1280" @{ HUB_SHOT_QM = "main"; HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_PAD = "wait,up,up,right,right,right,down,right,right,down,right"; HUB_SHOT_SECS = "8" }
