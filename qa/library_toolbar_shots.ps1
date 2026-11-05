# Headless screenshots of the All Games toolbar at UI sizes 80..200% (search box / chips must never overlap).
# Usage: powershell -NoProfile -File qa\library_toolbar_shots.ps1 [outdir] [sizes] [scales]   (BMPs are also converted to PNG)
param([string]$Out = "$env:TEMP\library_toolbar_shots", [string]$Sizes = '1280x800,1600x1000', [string]$Scales = '100,150,200', [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\RetroHub.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$vault = Join-Path $Out 'vault'
Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
$env:HUB_SHOT_VAULT = $vault
$env:HUB_SHOT_FAKE_LIB = '500'; $env:HUB_SHOT_FAKE_AZ = '1'; $env:HUB_SHOT_FAKE_ART = '1'; $env:HUB_SHOT_LIBVIEW = 'grid'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:HUB_SHOT_SECS = '4'
if ($Extra) { foreach ($kv in $Extra.Split(';')) { $p = $kv.Split('='); Set-Item "env:$($p[0])" $p[1] } }
foreach ($size in $Sizes.Split(',')) {
  foreach ($sc in $Scales.Split(',')) {
    $env:HUB_SHOT_SIZE = $size
    $env:HUB_UI_SCALE = $sc
    $env:HUB_SCREENSHOT = Join-Path $Out "toolbar-$sc-$size.bmp"
    $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force }
    Write-Host "$sc% $size -> $($env:HUB_SCREENSHOT) $(Test-Path $env:HUB_SCREENSHOT)"
  }
}
python -c "import sys,glob;from PIL import Image;[Image.open(f).save(f[:-4]+'.png') for f in glob.glob(sys.argv[1]+'/*.bmp')]" $Out
