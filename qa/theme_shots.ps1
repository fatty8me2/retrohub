# Headless theme screenshots of the friend-games pages and Wrapped (fake friends, no network, nothing visible).
# Usage: qa\theme_shots.ps1 -Sandbox <copy of bin\Release without userdata> -Out <dir> [-Theme midnight|light|...]
param([Parameter(Mandatory)][string]$Sandbox, [Parameter(Mandatory)][string]$Out, [string]$Theme = "midnight", [string]$Secs = "10", [string]$Only = "", [string]$Suite = "")
$ErrorActionPreference = "Stop"
$exe = Join-Path $Sandbox "RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
# -Suite contrast: library home, a game page and Settings in EVERY theme at 1280x720 (text contrast before/after comparisons).
if ($Suite -eq "contrast") {
  Add-Type -AssemblyName System.Drawing
  foreach ($t in @("midnight", "oled", "light", "atomic", "gamecube", "famicom")) {
    $views = @{ home = @{}; game = @{ HUB_SHOT_FAKE_LIB = "8"; HUB_SHOT_DETAILS = "fake:2" }; settings = @{ HUB_SHOT_PAGE = "settings:general" } }
    foreach ($v in $views.Keys) {
      $set = @{ RETROHUB_NO_SHELL = "1"; RETROHUB_QA = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; SDL_VIDEODRIVER = "dummy";
                HUB_SHOT_VAULT = (Join-Path $Sandbox "vault"); HUB_THEME = $t; HUB_SHOT_SIZE = "1280x720"; HUB_SHOT_SECS = $Secs;
                HUB_SHOT_FAKE_LIB = "all"; HUB_SHOT_CONTINUE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$($t)_$v.bmp") }
      foreach ($k in $views[$v].Keys) { $set[$k] = $views[$v][$k] }
      foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
      $p = Start-Process $exe -PassThru -WindowStyle Hidden
      if (-not $p.WaitForExit(90000)) { $p.Kill() }
      foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
      Write-Host "shot $($t)_$v"
    }
  }
  Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
  return
}
function Shot($name, $extra) {
  if ($Only -ne "" -and ($Only -split ",") -notcontains $name) { return }
  $set = @{ RETROHUB_NO_SHELL = "1"; RETROHUB_QA = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; SDL_VIDEODRIVER = "dummy";
            HUB_SHOT_VAULT = (Join-Path $Sandbox "vault"); HUB_THEME = $Theme;
            HUB_SCREENSHOT = (Join-Path $Out "$name.bmp"); HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_SECS = $Secs;
            HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FAKE_LIB = "all"; HUB_SHOT_FAKE_FRIEND_LIBS = "40" }
  foreach ($k in $extra.Keys) { $set[$k] = $extra[$k] }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(90000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  Write-Host "shot $name"
}
Shot "home" @{}
Shot "all" @{ HUB_START_VIEW = "all" }
Shot "all_gc" @{ HUB_START_VIEW = "all"; HUB_START_SYS = "gc" }
Shot "details" @{ HUB_START_VIEW = "all"; HUB_SHOT_GHOST_OPEN = "3" }
$cl = @{ HUB_START_VIEW = "all"; HUB_SHOT_LIBVIEW = "grid" }
Shot "grid" $cl
Shot "grid_gc" ($cl + @{ HUB_START_SYS = "gc" })
Shot "grid_det" ($cl + @{ HUB_SHOT_GHOST_OPEN = "3" })
Shot "shelf" @{ HUB_START_VIEW = "all"; HUB_SHOT_LIBVIEW = "shelf" }
Shot "wrapped" @{ HUB_SHOT_WRAPPED = "year"; HUB_SHOT_WRAPPED_FAKE = "1" }
Shot "wrapped3" @{ HUB_SHOT_WRAPPED = "year"; HUB_SHOT_WRAPPED_FAKE = "1"; HUB_SHOT_WRAPPED_CARD = "3" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
