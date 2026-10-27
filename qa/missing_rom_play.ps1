# Real Play path -> "Can't find game" popup, headless and muted (scratch vault, no network, no window).
# RETROHUB_AUTOTEST=play presses the same play_entry() the Play button calls on a fake library entry whose file does not exist;
# HUB_REAL_ROMCHECK=1 switches rom_missing() from its headless stub to the real file check. PASS when autotest.log says the popup opened.
# Usage: qa\missing_rom_play.ps1 [outdir]      Exit code 1 on failure.
param([string]$Out = "$env:TEMP\missing_rom_play")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $root "build\bin\Release\userdata\autotest.log"
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item (Join-Path $root "build\bin\Release\userdata\hub.json") -ErrorAction SilentlyContinue
$set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "play.bmp");
          HUB_SHOT_VAULT = (Join-Path $Out "vault"); HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "8"; HUB_SHOT_FAKE_LIB = "14";
          HUB_REAL_ROMCHECK = "1"; RETROHUB_AUTOTEST = "play"; RETROHUB_AUTOTEST_GAME = "fake:0" }
foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
$p = Start-Process $exe -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(120000)) { $p.Kill() }
foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
$txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
if ($txt -match "play: missing-rom popup opened") { Write-Host "PASS Play on a missing file opens the popup"; exit 0 }
Write-Host "FAIL no popup. autotest.log:"; Write-Host $txt
exit 1
