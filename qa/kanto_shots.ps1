# Headless screenshots of the Kanto Together UI (library card, room status, trade panel, setup, how-to). No window is shown, no network.
# usage: qa\kanto_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\kt_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"
function Shot($name, $vars) {
  foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
Shot "card"       @{ HUB_SHOT_KT = "idle"; HUB_SELECT_GAME = "gba:kanto-together"; HUB_SHOT_PAGE = "" }
Shot "room_conn"  @{ HUB_SHOT_KT = "connecting"; HUB_SHOT_SIZE = "1280x900"; HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_GAME = "kanto" }
Shot "room_trade" @{ HUB_SHOT_KT = "trade"; HUB_SHOT_SIZE = "1280x900"; HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_GAME = "kanto" }
Shot "setup"      @{ HUB_SHOT_KT = "installing" }
Shot "howto"      @{ HUB_SHOT_KT = "howto" }
Shot "guest_joinfail" @{ HUB_SHOT_KT = "joinfail"; HUB_SHOT_SIZE = "1280x1250"; HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_GAME = "kanto" }
Shot "host_peerfail"  @{ HUB_SHOT_KT = "peerfail"; HUB_SHOT_SIZE = "1280x1250"; HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_GAME = "kanto" }
