# Headless screenshots of room states: disconnected Rejoin strip, Kanto block reason (3 players), full-room invite. No window, no network.
# usage: qa\online_shots.ps1 [-Out <dir>]
param([string]$Out = "$env:TEMP\online_shots")
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $vars) {
  $base = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "3"; HUB_SHOT_SIZE = "1280x900"; HUB_SCREENSHOT = "$Out\$name.bmp" }
  foreach ($k in $vars.Keys) { $base[$k] = $vars[$k] }
  foreach ($k in $base.Keys) { Set-Item "env:$k" $base[$k] }
  $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  foreach ($k in $base.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Output "$name -> $(Test-Path "$Out\$name.bmp")"
}
Shot "rejoin"      @{ HUB_SHOT_REJOIN = "Connection to the lobby server was lost" }
Shot "kanto_block" @{ HUB_SHOT_KT = "block"; HUB_SHOT_FAKE_ROOM = "1"; HUB_SHOT_ROOM_N = "3"; HUB_SHOT_ROOM_GAME = "kanto" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
