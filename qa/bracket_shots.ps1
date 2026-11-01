# Headless bracket screenshots (Plan 4 P7-P8): page | champion | ladder at 1280x800 and 1600x1000. No window, muted.
$exe = Join-Path $PSScriptRoot '..\build\bin\Release\RetroHub.exe'
$out = Join-Path $PSScriptRoot 'shots\bracket'
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($size in '1280x800','1600x1000') { foreach ($m in 'page','champion','ladder') {
  $env:HUB_SHOT_BRACKET = $m; $env:HUB_SHOT_SIZE = $size; $env:HUB_SCREENSHOT = Join-Path $out "$m-$size.bmp"; $env:SSB64_MUTE = '1'
  & $exe | Out-Null
} }
Remove-Item Env:HUB_SHOT_BRACKET, Env:HUB_SHOT_SIZE, Env:HUB_SCREENSHOT
