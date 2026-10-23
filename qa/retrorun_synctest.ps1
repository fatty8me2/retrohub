# Rollback SyncTest for every retrorun core (headless, offline, muted).
# Every frame: roll back -Depth frames, re-simulate, compare output (and state hash) with the first run.
# Usage: powershell -File qa\retrorun_synctest.ps1 -RomDir <dir with Ruby.gba, Red.gb, Crystal.gbc, ...> [-Frames 3000] [-Depth 8]
param(
    [Parameter(Mandatory = $true)][string]$RomDir,
    [int]$Frames = 3000,
    [int]$Depth = 8
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$cores = Join-Path $root 'deps\cores'
$bin = Join-Path $root 'build\bin\Release'
$out = Join-Path $root 'qa\out\synctest'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:RETRORUN_MUTE = '1'
$cases = @(
    @{ n = 'mgba-Ruby';        core = "$cores\mgba_libretro.dll";            rom = 'Ruby.gba';      sys = 'gba'; extra = @() },
    @{ n = 'mgba-Emerald';     core = "$cores\mgba_libretro.dll";            rom = 'Emerald.gba';   sys = 'gba'; extra = @() },
    @{ n = 'mgba-FireRed';     core = "$cores\mgba_libretro.dll";            rom = 'FireRed.gba';   sys = 'gba'; extra = @() },
    @{ n = 'gambatte-Red';     core = "$cores\gambatte_libretro.dll";        rom = 'Red.gb';        sys = 'gb';  extra = @() },
    @{ n = 'gambatte-Crystal'; core = "$cores\gambatte_libretro.dll";        rom = 'Crystal.gbc';   sys = 'gbc'; extra = @() },
    @{ n = 'sameboy-Red';      core = "$cores\sameboy_libretro.dll";         rom = 'Red.gb';        sys = 'gb';  extra = @() },
    @{ n = 'sameboy-Crystal';  core = "$cores\sameboy_libretro.dll";         rom = 'Crystal.gbc';   sys = 'gbc'; extra = @() },
    @{ n = 'sameboy-link-Gold'; core = "$cores\sameboy_libretro.dll";        rom = 'Gold.gbc';      sys = 'gbc'; extra = @('--link', '--players', '2', '--ports', '0,1,-1,-1') },
    @{ n = 'mgba_link-Ruby';   core = "$bin\mgba_link_libretro.dll";         rom = 'Ruby.gba';      sys = 'gba'; extra = @('--link', '--players', '2', '--ports', '0,1,-1,-1') }
)
$fail = 0
foreach ($c in $cases) {
    $log = Join-Path $out ($c.n + '.log')
    $sv = Join-Path $out ('sv_' + $c.n)
    New-Item -ItemType Directory -Force -Path $sv | Out-Null
    $args = @('--core', $c.core, '--rom', (Join-Path $RomDir $c.rom), '--system', $c.sys, '--save-dir', $sv, '--state-dir', $sv,
              '--headless', '--frames', $Frames, '--synctest', $Depth, '--log', $log) + $c.extra
    $p = Start-Process -FilePath $exe -ArgumentList $args -PassThru -WindowStyle Hidden -Wait
    $sum = (Select-String -Path $log -Pattern 'SYNCTEST' | ForEach-Object { $_.Line -replace '^\[?[^\]]*\]\s*', '' }) -join "`n   "
    Write-Output ("{0}: exit {1}`n   {2}" -f $c.n, $p.ExitCode, $sum)
    if ($p.ExitCode -ne 0) { $fail++ }
}
if ($fail) { Write-Output "SYNCTEST FAILED ($fail)"; exit 1 }
Write-Output 'SYNCTEST ALL PASS'
