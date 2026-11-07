# Rerunnable QA for Hookshot <-> Lichii saves through one vault (headless, temp dirs only).
# Usage: powershell -NoProfile -File qa\hookshot_saves.ps1 [-Real <backup lichii-nand folder>]
# Runs the harness from this worktree's own build folder, so Hookshot's data folder is <build>\bin\Release\userdata.
param(
    [string]$Real = 'C:\Users\fatty\AppData\Local\RetroHub\save-backups\oot-switch2-before-hookshot-2026-11-07\lichii-nand'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\hub_hookshot_saves_qa.exe'
if (-not (Test-Path $exe)) {
    Write-Error "Build first: cmake --build build --config Release --target hub_hookshot_saves_qa"
    exit 2
}
if (-not (Test-Path $Real)) { Write-Error "Real save backup not found: $Real"; exit 2 }
$env:SSB64_MUTE = '1'
& $exe $Real
exit $LASTEXITCODE
