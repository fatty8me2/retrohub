# Rollback netplay under 4-5 s network stalls (HUBNET_SIM_SPIKE_*), headless + muted, LOCAL server only.
# Usage: powershell -File qa\rb_stall_online.ps1 [-Server ws://127.0.0.1:11700] [-Iter 2]
# SNES 4-player multitap (relay and direct), NES/SNES 2-player, plus an every-peer spike: nobody may desync, be dropped, or exit non-zero.
param([string]$Server = 'ws://127.0.0.1:8761', [int]$Iter = 1, [switch]$Relay4P)     # 4P relay scenarios are slow (opt in)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$fail = 0
$env:HUBNET_SIM_ENABLE = '1'; $env:HUBNET_SIM_LATENCY_MS = '40'; $env:HUBNET_SIM_JITTER_MS = '30'; $env:HUBNET_SIM_LOSS_PCT = '2'
$env:HUBNET_SIM_SPIKE_MS = '4600'; $env:HUBNET_SIM_SPIKE_AT_MS = '12000'; $env:HUBNET_SIM_SPIKE_EVERY_MS = '16000'
function Run($label, $args2) {
    Write-Output "== $label"
    $out = & powershell -NoProfile -File "$PSScriptRoot\retrorun_online.ps1" -Server $Server -Rollback @args2 2>&1
    $out | Where-Object { $_ -match 'slot \d rc|RESULT|FAIL|DESYNC' } | ForEach-Object { Write-Output $_ }
    if ($LASTEXITCODE -ne 0) { $script:fail++ }
}
for ($i = 0; $i -lt $Iter; $i++) {
    foreach ($relay in 0, 1) {
        # the relay already adds its own latency/HOL blocking (in-order TCP) and a 400 frames/s room cap: keep the simulated base link mild there
        if ($relay) { $env:HUBNET_FORCE_RELAY = '1'; $env:HUBNET_SIM_LATENCY_MS = '15'; $env:HUBNET_SIM_JITTER_MS = '10'; $env:HUBNET_SIM_LOSS_PCT = '0' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue; $env:HUBNET_SIM_LATENCY_MS = '40'; $env:HUBNET_SIM_JITTER_MS = '30'; $env:HUBNET_SIM_LOSS_PCT = '2' }
        foreach ($slot in -1, 3) {
            if ($relay -and -not $Relay4P) { continue }
            $env:HUBNET_SIM_SPIKE_SLOT = "$slot"   # -1: every peer stalls at once
            $t = "r${relay}s${slot}i$i"
            Run "SNES 4P relay=$relay spike-slot=$slot" @('-Core', 'snes9x_libretro.dll', '-Rom', 'deps\testroms\netplay.sfc', '-System', 'snes', '-Frames', 2400, '-Players', 4, '-Ports', '0,1,2,3', '-Tag', "stall4$t")
        }
        $env:HUBNET_SIM_SPIKE_SLOT = '1'
        Run "NES 2P relay=$relay spike-slot=1" @('-Core', 'nestopia_libretro.dll', '-Rom', 'deps\testroms\netplay.nes', '-System', 'nes', '-Frames', 2400, '-Players', 2, '-Tag', "stall2r${relay}i$i")
    }
}
Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue
if ($fail) { Write-Output "RB STALL ONLINE QA: FAIL ($fail)"; exit 1 }
Write-Output 'RB STALL ONLINE QA: ALL PASS'; exit 0
