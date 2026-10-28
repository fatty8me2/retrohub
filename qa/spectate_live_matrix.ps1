# Live-join spectate matrix (headless, muted, LOCAL worker only): 2 players + 1 late spectator per run, through qa\spectate_online.ps1.
#   powershell -NoProfile -File qa\spectate_live_matrix.ps1 [-Port 8795] [-LateSec 20] [-Frames 3000]
# Each run must report the spectator live from a snapshot within 3 s, agreeing hashes and no player frame gap over 50 ms while it joined.
param([int]$Port = 8795, [int]$LateSec = 20, [int]$Frames = 3000)
$ErrorActionPreference = 'Continue'
$q = $PSScriptRoot
$runs = @(
    @{ Tag = 'lv-snes'; A = @('-Mode', 'rollback', '-Core', 'snes9x_libretro.dll', '-Rom', 'deps/testroms/snes_test.sfc', '-System', 'snes', '-ExpectLive') },
    @{ Tag = 'lv-snes-raw'; Env = 'RETRORUN_TEST_SPECTATE_RAW'; A = @('-Mode', 'rollback', '-Core', 'snes9x_libretro.dll', '-Rom', 'deps/testroms/snes_test.sfc', '-System', 'snes', '-ExpectLive') },
    @{ Tag = 'lv-snes-relay'; A = @('-Mode', 'rollback', '-Core', 'snes9x_libretro.dll', '-Rom', 'deps/testroms/snes_test.sfc', '-System', 'snes', '-ExpectLive', '-Relay') },
    @{ Tag = 'lv-nes'; A = @('-Mode', 'rollback', '-Core', 'nestopia_libretro.dll', '-Rom', 'deps/testroms/nes_test.nes', '-System', 'nes', '-ExpectLive') },
    @{ Tag = 'lv-gba'; A = @('-Mode', 'lockstep', '-Core', 'mgba_libretro.dll', '-Rom', 'deps/testroms/sram.gba', '-System', 'gba', '-ExpectLive') },
    @{ Tag = 'lv-gba-raw-relay'; Env = 'RETRORUN_TEST_SPECTATE_RAW'; A = @('-Mode', 'lockstep', '-Core', 'mgba_libretro.dll', '-Rom', 'deps/testroms/sram.gba', '-System', 'gba', '-ExpectLive', '-Relay') },
    @{ Tag = 'lv-gbc-rb'; A = @('-Mode', 'rollback', '-Core', 'sameboy_rb_libretro.dll', '-Rom', 'deps/testroms/cgb-acid2.gbc', '-System', 'gbc', '-ExpectLive') },
    @{ Tag = 'lv-gba-prematch'; Late = 0; A = @('-Mode', 'lockstep', '-Core', 'mgba_libretro.dll', '-Rom', 'deps/testroms/sram.gba', '-System', 'gba') },
    @{ Tag = 'lv-snes-prematch'; Late = 0; A = @('-Mode', 'rollback', '-Core', 'snes9x_libretro.dll', '-Rom', 'deps/testroms/snes_test.sfc', '-System', 'snes') }
)
$summary = @()
foreach ($r in $runs) {
    $late = if ($r.ContainsKey('Late')) { $r.Late } else { $LateSec }
    if ($r.Env) { Set-Item -Path "env:$($r.Env)" -Value '1' }
    $env:HUBNET_FORCE_RELAY = $null
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $q 'spectate_online.ps1') @($r.A + @('-Frames', $Frames, '-LateSec', $late, '-Port', $Port, '-Tag', $r.Tag)) 2>&1
    if ($r.Env) { Remove-Item -Path "env:$($r.Env)" }
    Write-Output "===== $($r.Tag)"
    $out | Where-Object { $_ -notmatch 'save:' } | ForEach-Object { Write-Output "  $_" }
    $res = ($out | Where-Object { $_ -match 'RESULT:' } | Select-Object -Last 1)
    $summary += "{0,-20} {1}" -f $r.Tag, $res
}
Write-Output '===== summary'
$summary | ForEach-Object { Write-Output $_ }
