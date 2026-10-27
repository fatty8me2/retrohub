# Headless TV-mode screenshots (HUB_TV=1, dummy video driver, fake library/friends, muted, no network).
# usage: qa\tv_shots.ps1 [-Out <dir>]
param([string]$Out = "$env:TEMP\tv_shots")
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_TV = "1"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "4";
            HUB_SHOT_FAKE_LIB = "8"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp"); HUB_SHOT_SIZE = $size } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
foreach ($size in @("1920x1080", "3840x2160")) {
  Shot "home" $size @{}
  Shot "details" $size @{ HUB_SELECT_GAME = "fake:1"; HUB_SHOT_KEYS = "i" }
  Shot "settings" $size @{ HUB_START_PAGE = "4" }
}
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
