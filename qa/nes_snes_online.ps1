# NES (Mesen) + SNES (snes9x) rollback netplay QA, headless + muted, LOCAL server only (qa\_guard.ps1).
# Usage: powershell -File qa\nes_snes_online.ps1 [-Server ws://127.0.0.1:11310]
# Generates deps\testroms\netplay.nes / .sfc (qa\make_nesnes_roms.py) when missing. Runs: synctest, direct 2P, relay 2P/4P, 3P, 4P multitap, guest drop, host leave.
param([string]$Server = 'ws://127.0.0.1:8761')
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$root = Split-Path -Parent $PSScriptRoot
$roms = Join-Path $root 'deps\testroms'
if (-not (Test-Path "$roms\netplay.nes") -or -not (Test-Path "$roms\netplay.sfc")) { python "$PSScriptRoot\make_nesnes_roms.py" $roms }
$env:RETRORUN_MUTE = '1'
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$fail = 0
$sets = @(@{ n = 'nes'; core = 'mesen_libretro.dll'; rom = 'deps\testroms\netplay.nes'; sys = 'nes' }, @{ n = 'snes'; core = 'snes9x_libretro.dll'; rom = 'deps\testroms\netplay.sfc'; sys = 'snes' })
foreach ($s in $sets) {
    $sv = Join-Path $root ('qa\out\nss_' + $s.n); New-Item -ItemType Directory -Force $sv | Out-Null
    $p = Start-Process $exe -ArgumentList @('--core', (Join-Path $root ('deps\cores\' + $s.core)), '--rom', (Join-Path $root $s.rom), '--system', $s.sys, '--save-dir', $sv, '--state-dir', $sv, '--headless', '--frames', 1500, '--synctest', 8, '--fuzz-input', '--log', "$sv\sync.log") -Wait -PassThru -WindowStyle Hidden
    Write-Output ($s.n + ' synctest rc=' + $p.ExitCode); if ($p.ExitCode -ne 0) { $fail++ }
}
function Run($label, $s, $extra) {
    Write-Output "== $($s.n) $label"
    $out = & powershell -NoProfile -File "$PSScriptRoot\retrorun_online.ps1" -Server $Server -Core $s.core -Rom $s.rom -System $s.sys -Rollback @extra 2>&1
    $out | Where-Object { $_ -match 'slot \d rc|RESULT|FAIL' } | ForEach-Object { Write-Output $_ }
    if ($LASTEXITCODE -ne 0) { $script:fail++ }
}
foreach ($s in $sets) {
    Run 'direct 2P' $s @('-Frames', 2400, '-Players', 2, '-Tag', ($s.n + '2'))
    $env:HUBNET_FORCE_RELAY = '1'; Run 'relay 2P' $s @('-Frames', 1800, '-Players', 2, '-Tag', ($s.n + '2r')); Remove-Item Env:HUBNET_FORCE_RELAY
    Run 'guest drop (2P)' $s @('-Frames', 3000, '-Players', 2, '-KillSlot', 1, '-KillAfterSec', 8, '-Tag', ($s.n + 'k'))
    Run 'host leaves' $s @('-Frames', 3000, '-Players', 2, '-KillSlot', 0, '-KillAfterSec', 8, '-Tag', ($s.n + 'h'))
}
$sn = $sets[1]
Run 'direct 3P multitap' $sn @('-Frames', 1800, '-Players', 3, '-Ports', '0,1,2,-1', '-Tag', 'snes3')
Run 'direct 4P multitap' $sn @('-Frames', 1800, '-Players', 4, '-Ports', '0,1,2,3', '-Tag', 'snes4')
$env:HUBNET_FORCE_RELAY = '1'; Run 'relay 4P multitap' $sn @('-Frames', 1800, '-Players', 4, '-Ports', '0,1,2,3', '-Tag', 'snes4r'); Remove-Item Env:HUBNET_FORCE_RELAY
Run '4P guest drop keeps playing' $sn @('-Frames', 3000, '-Players', 4, '-Ports', '0,1,2,3', '-KillSlot', 2, '-KillAfterSec', 8, '-ExpectDrop', '-Tag', 'snes4k')
if ($fail) { Write-Output "NES/SNES ONLINE QA: FAIL ($fail)"; exit 1 }
Write-Output 'NES/SNES ONLINE QA: ALL PASS'; exit 0
