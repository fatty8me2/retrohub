# Desync recovery (rollback resync) online QA, headless + muted, LOCAL server only.
# Usage: powershell -File qa\resync_online.ps1 [-Server ws://127.0.0.1:8761]
# Each scenario injects a desync into slot 1 (RETRORUN_TEST_DESYNC="1:<frame>,..."), direct and through the forced relay:
#   - recovered: both peers log "resync: N done", exit 0, and finish on identical state/output hashes
#   - give-up:   4 injections with a budget of 3 end the game with exit 5 on at least one peer (old behaviour once the budget is gone)
param([string]$Server = 'ws://127.0.0.1:8761')
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$fail = 0
$env:RB_HASH_INTERVAL = '60'
function Scenario($label, $inject, $expectResyncs, $giveUp, $core, $rom, $sys, $frames, $relay) {
    $tag = 'rs_' + ($label -replace '[^a-z0-9]', '')
    if ($relay) { $env:HUBNET_FORCE_RELAY = '1' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }
    $env:RETRORUN_TEST_DESYNC = $inject
    $out = & powershell -NoProfile -File "$PSScriptRoot\retrorun_online.ps1" -Server $Server -Rollback -Core $core -Rom $rom -System $sys -Frames $frames -Tag $tag 2>&1
    $dir = Join-Path (Split-Path -Parent $PSScriptRoot) "qa\out\$tag"
    $done = @(0, 1 | ForEach-Object { @(Select-String -Path (Join-Path $dir "slot$_.log") -Pattern 'resync: \d+ done').Count })
    $hashes = @($out | Select-String -Pattern 'final: frame (\d+) state-hash (\w+) output-hash (\w+)' | ForEach-Object { $_.Matches[0].Groups[1].Value + '/' + $_.Matches[0].Groups[2].Value + '/' + $_.Matches[0].Groups[3].Value })
    $rcs = @($out | Select-String -Pattern 'slot \d rc=(\d+)' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    $ok = $true
    if ($giveUp) {
        if (-not ($rcs -contains 5)) { $ok = $false }
        if (($done | Measure-Object -Maximum).Maximum -ne 3) { $ok = $false }
    } else {
        if ($done[0] -ne $expectResyncs -or $done[1] -ne $expectResyncs) { $ok = $false }
        if (@($rcs | Where-Object { $_ -ne 0 }).Count -gt 0 -or $rcs.Count -ne 2) { $ok = $false }
        if ($hashes.Count -ne 2 -or $hashes[0] -ne $hashes[1]) { $ok = $false }
    }
    Write-Output ("{0,-34} resyncs {1}/{2} rc {3} hashes {4} => {5}" -f $label, $done[0], $done[1], ($rcs -join ','), ($hashes -join ' | '), $(if ($ok) { 'PASS' } else { 'FAIL' }))
    if (-not $ok) { $script:fail++ }
}
foreach ($relay in $false, $true) {
    $r = if ($relay) { 'relay' } else { 'direct' }
    Scenario "nes 1 desync $r"      '1:300'                1 $false 'nestopia_libretro.dll' 'deps\testroms\nes_test.nes' 'nes' 1500 $relay
    Scenario "snes 2 desyncs $r"    '1:300,1:700'          2 $false 'snes9x_libretro.dll'   'deps\testroms\snes_test.sfc' 'snes' 1500 $relay
    Scenario "snes host+guest $r"   '0:300,1:700'          2 $false 'snes9x_libretro.dll'   'deps\testroms\snes_test.sfc' 'snes' 1500 $relay   # the host itself diverging: its confirmed state is still the authority
}
# incompressible state (test switch): 1177 chunks of 700 bytes through selective-repeat, direct and relayed
$env:RETRORUN_TEST_RESYNC_RAW = '1'
foreach ($relay in $false, $true) { Scenario ('snes raw 823 KB ' + $(if ($relay) { 'relay' } else { 'direct' })) '1:300' 1 $false 'snes9x_libretro.dll' 'deps/testroms/snes_test.sfc' 'snes' 1500 $relay }
Remove-Item Env:RETRORUN_TEST_RESYNC_RAW -ErrorAction SilentlyContinue
Scenario 'snes give-up after 3 (direct)' '1:200,1:500,1:800,1:1100' 3 $true 'snes9x_libretro.dll' 'deps\testroms\snes_test.sfc' 'snes' 1800 $false
Remove-Item Env:HUBNET_FORCE_RELAY, Env:RETRORUN_TEST_DESYNC -ErrorAction SilentlyContinue
if ($fail) { Write-Output "RESYNC ONLINE QA: FAIL ($fail)"; exit 1 }
Write-Output 'RESYNC ONLINE QA: ALL PASS'; exit 0
