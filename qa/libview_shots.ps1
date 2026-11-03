# Headless screenshots of the All Games layouts (grid / list / compact / showcase) with a 500-game fake library and procedural covers.
# Scratch vault, muted, SDL dummy driver: no window is ever shown.
# Usage: powershell -NoProfile -File qa\libview_shots.ps1 [outdir] [pad-sequence]   (BMPs are also converted to PNG)
param([string]$Out = "$env:TEMP\libview_shots", [string]$Sizes = '1280x800,1600x1000')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\RetroHub.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$vault = Join-Path $Out 'vault'
Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
Remove-Item -Force (Join-Path $root 'build\bin\Release\userdata\hub.json') -ErrorAction SilentlyContinue   # settings of earlier test runs (sort order, layout)
$env:HUB_SHOT_VAULT = $vault
$env:HUB_SHOT_FAKE_LIB = '500'; $env:HUB_SHOT_FAKE_AZ = '1'; $env:HUB_SHOT_FAKE_ART = '1'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$env:HUB_SHOT_SECS = '4'
foreach ($size in $Sizes.Split(',')) {
  foreach ($v in @('grid', 'list', 'compact', 'showcase')) {
    $env:HUB_SHOT_SIZE = $size
    $env:HUB_SHOT_LIBVIEW = $v
    $env:HUB_SCREENSHOT = Join-Path $Out "libview-$v-$size.bmp"
    & $exe | Out-Null
    Write-Host "$v $size -> $($env:HUB_SCREENSHOT) $(Test-Path $env:HUB_SCREENSHOT)"
  }
}
python -c "import sys,glob;from PIL import Image;[Image.open(f).save(f[:-4]+'.png') for f in glob.glob(sys.argv[1]+'/*.bmp')]" $Out
