# Headless screenshots of the Shelf library view (fake mixed-console library with procedural covers, long titles).
# Scratch vault, muted, SDL dummy driver: no window is ever shown. Pure-pad "focus" shots use HUB_SHOT_PAD.
# Usage: powershell -NoProfile -File qa\shelf_shots.ps1 [outdir] [sizes]   (BMPs are also converted to PNG)
param([string]$Out = "$env:TEMP\shelf_shots", [string]$Sizes = '1280x800,1600x1000')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\RetroHub.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$vault = Join-Path $Out 'vault'
Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
$env:HUB_SHOT_VAULT = $vault
$env:HUB_SHOT_FAKE_LIB = '260'; $env:HUB_SHOT_FAKE_SHELF = '1'; $env:HUB_SHOT_FAKE_ART = '1'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$env:HUB_SHOT_LIBVIEW = 'shelf'
$shots = @(
  @{ n = 'default';  env = @{} },
  @{ n = 'focus';    env = @{ HUB_SELECT_GAME = 'fake:3'; HUB_SHOT_PAD = '1' } },
  @{ n = 'carts';    env = @{ HUB_START_SYS = 'n64'; HUB_SHOT_FAKE_SHELF = 'n64' } },
  @{ n = 'carts-focus'; env = @{ HUB_START_SYS = 'gba'; HUB_SHOT_FAKE_SHELF = 'gba'; HUB_SHOT_PAD = 'right,right,right'; HUB_SHOT_SECS = '8' } },
  @{ n = 'cases';    env = @{ HUB_START_SYS = 'ps2'; HUB_SHOT_FAKE_SHELF = 'ps2' } },
  @{ n = 'cases-focus'; env = @{ HUB_START_SYS = 'ps2'; HUB_SHOT_FAKE_SHELF = 'ps2'; HUB_SHOT_PAD = 'right,right,right,right'; HUB_SHOT_SECS = '8' } },
  @{ n = 'mixed';    env = @{ HUB_SHOT_SHELF_MIXED = '1' } }
)
foreach ($size in $Sizes.Split(',')) {
  foreach ($s in $shots) {
    $saved = @{}
    foreach ($k in $s.env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $s.env[$k]) }
    $env:HUB_SHOT_SIZE = $size
    if (-not $s.env.ContainsKey('HUB_SHOT_SECS')) { $env:HUB_SHOT_SECS = '4' }
    $env:HUB_SCREENSHOT = Join-Path $Out "shelf-$($s.n)-$size.bmp"
    & $exe | Out-Null
    Write-Host "$($s.n) $size -> $($env:HUB_SCREENSHOT) $(Test-Path $env:HUB_SCREENSHOT)"
    foreach ($k in $s.env.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    $env:HUB_SHOT_SECS = $null
  }
}
python -c "import sys,glob;from PIL import Image;[Image.open(f).save(f[:-4]+'.png') for f in glob.glob(sys.argv[1]+'/*.bmp')]" $Out
