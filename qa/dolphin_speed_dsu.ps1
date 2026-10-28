# Dolphin emulation speed through the hub's virtual DSU pad, HEADLESS: Dolphin 2509 runs on a private hidden desktop (CreateDesktop),
# muted (No Audio Output), Null video, throwaway -u user dir, a generated homebrew .dol; no SendInput, no focus, nothing visible.
# The achieved speed is read from inside the guest (PowerPC timebase published to RAM, read from Dolphin's process memory), because
# Dolphin 2509 does not show the speed in its window title. Steps: 1x -> 0.5x -> 0.1x floor (+3 extra decreases) -> 1x -> 2x -> 1x.
# usage: qa\dolphin_speed_dsu.ps1 [-Build <bin\Release>] [-DolphinDir <deps\dolphin>] [-Work <scratch>] [-Video Null|D3D]
# exit: 0 pass, 1 fail, 77 skipped (no Dolphin)
param([string]$Build = "", [string]$DolphinDir = "", [string]$Work = "", [string]$Video = "Null")
$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not $DolphinDir) { $DolphinDir = Join-Path $root "deps\dolphin" }
if (-not $Work) { $Work = Join-Path $root "build\dolphin_speed_work" }
$exe = Join-Path $Build "hub_dolphin_speed_live.exe"
if (-not (Test-Path $exe)) { throw "build hub_dolphin_speed_live first: $exe" }
& $exe --dolphin $DolphinDir --work $Work --video $Video
exit $LASTEXITCODE
