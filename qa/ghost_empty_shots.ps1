# Headless screenshots: friends' greyed-out games with an EMPTY own library, and the Settings > Online diagnostics line.
param([string]$Out = "$PSScriptRoot\shots\ghost-empty")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name.bmp");
            HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_SECS = "10"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_LIB = "empty"; HUB_SHOT_FAKE_FRIEND_LIBS = "40" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
}
Shot "home_empty_with_ghosts" @{}
Shot "all_empty_with_ghosts" @{ HUB_START_VIEW = "all" }
Shot "empty_no_friends" @{ HUB_SHOT_FAKE_FRIENDS = "" }
Shot "settings_online" @{ HUB_SHOT_PAGE = "settings:1"; HUB_SHOT_SIZE = "1280x1100" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $_.FullName }
