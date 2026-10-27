# Headless screenshots of the Make-it-yours UI (rename / cover / custom shelves). Scratch vault only, muted, no visible window.
# Usage: powershell -NoProfile -File qa\custom_shots.ps1 [outdir]
param([string]$Out = "$env:TEMP\custom_shots")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\RetroHub.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$vault = Join-Path $Out 'vault'
Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
$prefs = '{"user_shelves":[{"name":"Co-op night","keys":["fake:0","fake:3","fake:4","fake:8"]},{"name":"Short games","keys":["fake:1","fake:2"]}],"renames":{"fake:0":"Best Game Ever"},"ts":{"user_shelves":1000,"renames":1000}}'
[IO.File]::WriteAllText((Join-Path $vault '_account\prefs.json'), $prefs)
$env:HUB_SHOT_VAULT = $vault
$env:HUB_SHOT_FAKE_LIB = '12'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$env:HUB_SHOT_SECS = '3'
$env:HUB_SHOT_SIZE = '1280x800'
foreach ($m in @('', 'customize', 'picker', 'manager')) {
    $name = if ($m) { $m } else { 'home' }
    if ($m) { $env:HUB_SHOT_CUSTOM = $m } else { Remove-Item Env:HUB_SHOT_CUSTOM -ErrorAction SilentlyContinue }
    $env:HUB_SCREENSHOT = Join-Path $Out "$name.bmp"
    & $exe | Out-Null
    Write-Host "$name -> $($env:HUB_SCREENSHOT) $(Test-Path $env:HUB_SCREENSHOT)"
}
