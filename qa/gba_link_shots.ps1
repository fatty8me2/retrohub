# Headless screenshots of the "Game Boy Advance link" row in a GameCube game's details (fake Metroid Prime; BIOS present / missing).
# usage: qa\gba_link_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]
param([string]$Out = "$env:TEMP\gba_link_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_FAKE_LIB = "8"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
$env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
$env:HUB_SELECT_GAME = "fake:2"; $env:HUB_SHOT_KEYS = "i"; $env:HUB_SHOT_TAB = "0"
foreach ($sz in "1280x800", "1600x1000") {
  foreach ($m in "bios", "nobios") {
    $env:HUB_SHOT_SIZE = $sz; $env:HUB_SHOT_GBALINK = $m
    $env:HUB_SCREENSHOT = "$Out\gbalink_${m}_$($sz.Split('x')[0]).bmp"
    $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force }
    Write-Output "$($env:HUB_SCREENSHOT) -> $(Test-Path $env:HUB_SCREENSHOT)"
  }
}
