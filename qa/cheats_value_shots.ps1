# Headless screenshots of the value editor (cheats with X/? placeholders): decimal, hex, option list, GB range hint. Nothing is shown;
# the vault is never touched (temp folder). usage: qa\cheats_value_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\cheat_value_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
Remove-Item -Recurse -Force "$env:TEMP\hub_cheats_shot" -ErrorAction SilentlyContinue
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
Shot "value_dec_n64" @{ HUB_SHOT_CHEATS = "value"; HUB_SHOT_SIZE = "1280x800" }
Shot "value_hex_n64" @{ HUB_SHOT_CHEATS = "valuehex"; HUB_SHOT_SIZE = "1280x800" }
Shot "value_list_n64" @{ HUB_SHOT_CHEATS = "valuelist"; HUB_SHOT_SIZE = "1600x1000" }
Shot "value_gb_hint" @{ HUB_SHOT_CHEATS = "value"; HUB_SHOT_CHEATS_SYS = "gb"; HUB_SHOT_SIZE = "1280x800" }
