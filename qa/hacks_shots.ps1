# Headless screenshots of the ROM hack UI (Plan 4 H2-H4): the add dialog, a game page with its Versions row, and the All Games tiles.
# usage: qa\hacks_shots.ps1 [-Out <dir>] [-Exe <RetroHub.exe>]     (default Out: qa\shots\hacks, 1280x800, muted, hidden window)
param([string]$Out = "$PSScriptRoot\shots\hacks", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_FAKE_LIB = "6"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
$env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:HUB_SHOT_SIZE = "1280x800"
function Shot($name, $vars) {
  foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
Shot "add" @{ HUB_SHOT_HACKS = "add" }
Shot "versions" @{ HUB_SHOT_HACKS = "versions"; HUB_SELECT_GAME = "fake:0" }
Shot "tile" @{ HUB_SHOT_HACKS = "tile"; HUB_SELECT_GAME = "fake:0+h00000000" }
