# Headless screenshots of the game details page (About / Saves / Screenshots / Settings tabs) for a rich and a bare game.
# usage: qa\details_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\details_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_FAKE_LIB = "6"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
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
foreach ($sz in "1280x800", "1600x1000") {
  $w = $sz.Split("x")[0]
  $rich = @{ HUB_SHOT_STATES = "1"; HUB_SHOT_GALLERY = "1"; HUB_SELECT_GAME = "fake:1"; HUB_SHOT_KEYS = "i"; HUB_SHOT_SIZE = $sz }
  Shot "rich_about_$w" ($rich + @{ HUB_SHOT_TAB = "0" })
  Shot "rich_saves_$w" ($rich + @{ HUB_SHOT_TAB = "1" })
  Shot "rich_shots_$w" @{ HUB_SHOT_GALLERY = "1"; HUB_SELECT_GAME = "fake:0"; HUB_SHOT_KEYS = "i"; HUB_SHOT_SIZE = $sz; HUB_SHOT_TAB = "2" }
  Shot "rich_settings_$w" ($rich + @{ HUB_SHOT_TAB = "3" })
  Shot "bare_$w" @{ HUB_SELECT_GAME = "fake:3"; HUB_SHOT_KEYS = "i"; HUB_SHOT_SIZE = $sz }
}
