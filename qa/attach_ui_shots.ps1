# Headless screenshots for the chat attachment cards and the screenshot viewer's "Send to a friend" picker (fake friends, no network).
# Usage: qa\attach_ui_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\attach_ui_shots")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $size, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_SECS = "5" } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name $size"
}
$size = "1600x1000"
Shot "chat_attach" $size @{ HUB_SHOT_FRIENDS = "1"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_ATTACH = "1"; HUB_SHOT_CHAT = "1" }
Shot "viewer_picker" $size @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_GALLERY = "1"; HUB_SHOT_VIEWER = "1"; HUB_SHOT_DETAILS = "fake:0"; HUB_SHOT_TAB = "2"; HUB_SHOT_FRIEND_PICK = "1" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out"
