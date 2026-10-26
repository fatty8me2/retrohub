# NES (Mesen) / SNES (Snes9x) headless smoke + rollback SyncTest on the generated homebrew ROMs (qa\make_nes_snes_roms.py).
# Muted, hidden, offline. Checks: frames run, .srm is written to the save dir, SyncTest passes.
# Usage: powershell -File qa\nes_snes_synctest.ps1 [-Frames 600] [-Depth 8]
param([int]$Frames = 600, [int]$Depth = 8)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$cores = Join-Path $root 'deps\cores'
$roms = Join-Path $root 'deps\testroms'
$out = Join-Path $root 'qa\out\nes_snes'
New-Item -ItemType Directory -Force -Path $out, $roms | Out-Null
& python (Join-Path $PSScriptRoot 'make_nes_snes_roms.py') $roms | Out-Null
$env:RETRORUN_MUTE = '1'; $env:SSB64_MUTE = '1'; $env:RETROHUB_START_HINT = '0'
$cases = @(
    @{ n = 'nestopia-nes'; core = "$cores\nestopia_libretro.dll"; rom = 'nes_test.nes'; sys = 'nes'; srm = 'nes_test.srm' },
    @{ n = 'snes9x-snes'; core = "$cores\snes9x_libretro.dll"; rom = 'snes_test.sfc'; sys = 'snes'; srm = 'snes_test.srm' }
)
$fail = 0
foreach ($c in $cases) {
    if (-not (Test-Path $c.core) -or -not (Test-Path $exe)) { Write-Output ("SKIP " + $c.n + " (core or retrorun missing)"); continue }
    foreach ($mode in @('run', 'synctest')) {
        $sv = Join-Path $out ("sv_" + $c.n + "_" + $mode)
        if (Test-Path $sv) { Remove-Item -Recurse -Force $sv }
        New-Item -ItemType Directory -Force -Path $sv | Out-Null
        $log = Join-Path $out ($c.n + "_" + $mode + '.log')
        $a = @('--core', $c.core, '--rom', (Join-Path $roms $c.rom), '--system', $c.sys, '--save-dir', $sv, '--state-dir', $sv,
               '--headless', '--frames', $Frames, '--log', $log)
        if ($mode -eq 'synctest') { $a += @('--synctest', $Depth) }
        $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden -Wait
        $srm = Join-Path $sv $c.srm
        $srmOk = (Test-Path $srm) -and ((Get-Item $srm).Length -gt 0)
        $sum = (Select-String -Path $log -Pattern 'SYNCTEST|frames' | Select-Object -Last 2 | ForEach-Object { $_.Line }) -join ' | '
        Write-Output ("{0} {1}: exit {2} srm={3}  {4}" -f $c.n, $mode, $p.ExitCode, $srmOk, $sum)
        if ($p.ExitCode -ne 0 -or -not $srmOk) { $fail++ }
    }
}
if ($fail) { Write-Output "NES/SNES SMOKE FAILED ($fail)"; exit 1 }
Write-Output 'NES/SNES SMOKE PASS'
