# Headless screenshots of the game info block in the details modal, on RECORDED Wikidata/Wikipedia fixtures (no live network).
# usage: qa\gameinfo_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\gameinfo_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$cache = "$env:TEMP\hub_gameinfo_shot_cache"
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_FAKE_LIB = "1"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "4"; $env:RETROHUB_QA = "1"
$env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
$env:HUB_GAMEINFO_FIXTURES = "$PSScriptRoot\..\src\hub\testdata\gameinfo"
$games = "n64;Super Smash Bros.;NALE|gba;Pokemon Emerald;BPEE|gc;Super Smash Bros. Melee;GALE01|ps1;Crash Bash;SCUS-94570|n64;The Legend of Zelda: Ocarina of Time;CZLE|n64;Zzqx Nonexistent Game;"
$names = "smash64", "emerald", "melee", "crashbash", "zelda", "unknown"
function Shot($name, $idx, $size, $keys) {
  Remove-Item -Recurse -Force $cache -ErrorAction SilentlyContinue
  $env:HUB_GAMEINFO_CACHE = $cache
  $env:HUB_SHOT_INFO = $games; $env:HUB_SELECT_GAME = "fake:info$idx"; $env:HUB_SHOT_SIZE = $size; $env:HUB_SHOT_KEYS = $keys
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
for ($i = 0; $i -lt $names.Count; $i++) {
  Shot "$($names[$i])_1280" $i "1280x800" "i"
  Shot "$($names[$i])_1600" $i "1600x1000" "i"
}
# no fixtures and no network: the offline-only look (libretro facts, "loads when you are online" note)
$env:HUB_GAMEINFO_FIXTURES = ""
Shot "offline_smash64_1280" 0 "1280x800" "i"
