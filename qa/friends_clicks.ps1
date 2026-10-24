# Headless friends-panel click checks (no visible window, no network, fake friends list).
# Usage: qa\friends_clicks.ps1 [outdir]      Exit code 1 if any click did not hit its intended action.
# Injects real mouse events via HUB_SHOT_CLICK (x,y;rx,y;... r prefix = right click) and asserts on the
# HUB_SHOT_TRACE action log (chat/join/invite/copy/remove/block/accept/decline). Coordinates are for 1280x800.
param([string]$Out = "$env:TEMP\friends_clicks")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\RetroHub.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$script:fail = 0
function Run($name, $clicks, $expect, $size = "1280x800") {
  $trace = Join-Path $Out "$name.trace"
  Remove-Item $trace -ErrorAction SilentlyContinue
  $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = (Join-Path $Out "$name-$size.bmp");
            HUB_SHOT_SIZE = $size; HUB_SHOT_FRIENDS = "1"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "4";
            HUB_SHOT_CLICK = $clicks; HUB_SHOT_TRACE = $trace }
  foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
  $p = Start-Process $exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  $got = if (Test-Path $trace) { (Get-Content $trace) -join "|" } else { "" }
  if ($got -eq $expect) { Write-Host "PASS $name -> '$got'" } else { Write-Host "FAIL $name expected '$expect' got '$got'"; $script:fail++ }
}
# Mia (in lobby, joinable) row at y=533; hover buttons: Join 1159, Message 1201, More 1230.
Run "row_left"        "1080,533"                       ""
Run "row_left_msg"    "1080,533;1136,552"              "chat miauid"
Run "row_right_msg"   "r1080,533;1136,552"             "chat miauid"
Run "row_left_join"   "1080,533;1140,587"              "join K7M2QX"
Run "row_left_copy"   "1080,533;1160,622"              "copy miauid"
Run "row_left_remove" "1080,533;1154,673"              "remove miauid"
Run "row_left_block"  "1080,533;1125,708"              "block miauid"
Run "btn_join"        "1159,533"                       "join K7M2QX"
Run "btn_msg"         "1201,533"                       "chat miauid"
Run "btn_more_msg"    "1230,533;1136,552"              "chat miauid"
Run "req_accept"      "1144,341"                       "accept oliuid"
Run "req_decline"     "1216,341"                       "decline oliuid"
Run "dead_zone"       "1100,440"                       ""
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$Out\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $png = $_.FullName -replace '\.bmp$', '.png'; $i.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose() }
Write-Host "screenshots in $Out; failures: $($script:fail)"
exit ([int]($script:fail -gt 0))
