# Headless screenshots of the ghost details + Settings > Online against the local GamePack fake (no real network, muted, hidden).
# Usage: qa\gamepack_shots.ps1 [outdir]   (starts the fake on 127.0.0.1:18766)
param([string]$Out = "$env:TEMP\gamepack_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$srv = Start-Process python -ArgumentList "`"$root\qa\gamepack_fake_server.py`"","18766" -PassThru -WindowStyle Hidden
Start-Sleep 1
function Shot($name, $mode, $extra) {
  Invoke-WebRequest "http://127.0.0.1:18766/__mode?packs=$mode" -UseBasicParsing | Out-Null
  $set = @{ RETROHUB_NO_SHELL="1"; RETROHUB_QA="1"; RETROHUB_GP_BASE="127.0.0.1:18766"; SSB64_MUTE="1"; RETRORUN_MUTE="1"; N64RUN_MUTE="1"
            HUB_SCREENSHOT=(Join-Path $Out "$name.bmp"); HUB_SHOT_SIZE="1280x800"; HUB_SHOT_SECS="12"; HUB_SHOT_FAKE_FRIENDS="1"; HUB_SHOT_FAKE_FRIEND_LIBS="40"; HUB_START_VIEW="all"; HUB_START_SYS="gba" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name"
}
try {
  Shot "ghost_listed_ok" "ok" @{ HUB_SHOT_GHOST_OPEN="0" }
  Shot "ghost_list404" "404" @{ HUB_SHOT_GHOST_OPEN="0" }
  Shot "settings_online" "ok" @{ HUB_START_PAGE="4"; HUB_SHOT_TAB="" }
} finally { $srv.Kill() }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
