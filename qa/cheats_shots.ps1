# Headless screenshots of the Cheats panel (list, add custom code, GBA, GB, guest-locked). No window is shown; the vault is never touched
# (headless runs keep cheat choices in %TEMP%\hub_cheats_shot).
# usage: qa\cheats_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\cheat_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item -Recurse -Force "$env:TEMP\hub_cheats_shot" -ErrorAction SilentlyContinue
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
function Shot($name, $vars) {
  foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
Shot "list_n64_1280"  @{ HUB_SHOT_CHEATS = "list"; HUB_SHOT_SIZE = "1280x800" }
Shot "add_n64_1600"   @{ HUB_SHOT_CHEATS = "add"; HUB_SHOT_SIZE = "1600x1000" }
Shot "list_gba_1600"  @{ HUB_SHOT_CHEATS = "list"; HUB_SHOT_CHEATS_SYS = "gba"; HUB_SHOT_SIZE = "1600x1000" }
Shot "list_gb_1280"   @{ HUB_SHOT_CHEATS = "list"; HUB_SHOT_CHEATS_SYS = "gb"; HUB_SHOT_SIZE = "1280x800" }
Shot "locked_guest"   @{ HUB_SHOT_CHEATS = "locked"; HUB_SHOT_SIZE = "1280x800" }
