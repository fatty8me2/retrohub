# Headless screenshots: Play Online (friends' rooms / none, help open / collapsed) and the invite toast (full / shrunk pill). No network, muted, hidden.
param([string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "qa\shots\ui-c"))
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name, $extra) {
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name.bmp");
            HUB_SHOT_SIZE = "1600x1000"; HUB_SHOT_SECS = "5"; HUB_SHOT_FAKE_LIB = "8" } + $extra
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name"
}
Shot "online_rooms_help_open" @{ HUB_SHOT_PAGE = "online"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_PLAYED_ONLINE = "0" }
Shot "online_rooms_help_collapsed" @{ HUB_SHOT_PAGE = "online"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_PLAYED_ONLINE = "1" }
Shot "online_norooms_help_open" @{ HUB_SHOT_PAGE = "online"; HUB_SHOT_PLAYED_ONLINE = "0" }
Shot "online_norooms_help_collapsed" @{ HUB_SHOT_PAGE = "online"; HUB_SHOT_PLAYED_ONLINE = "1" }
Shot "invite_full" @{ HUB_SHOT_FAKE_FRIENDS = "1" }
Shot "invite_pill" @{ HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_INVITE_AGE = "12" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose(); Remove-Item $_.FullName }
