# Game Boy / GBA rollback SyncTest on generated homebrew ROMs (qa\make_gb_gba_roms.py): no commercial game needed (audit netcore-M4).
# Muted, hidden, offline. Per core: the frames run, the .srm is written, the SyncTest (roll back -Depth frames every frame, re-simulate,
# compare) passes. Cores that are not installed are skipped.
# Usage: powershell -File qa\gb_gba_synctest.ps1 [-Frames 900] [-Depth 8]
param([int]$Frames = 900, [int]$Depth = 8)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$cores = Join-Path $root 'deps\cores'
$bin = Join-Path $root 'build\bin\Release'
$roms = Join-Path $root 'deps\testroms'
$out = Join-Path $root 'qa\out\gb_gba'
New-Item -ItemType Directory -Force -Path $out, $roms | Out-Null
& python (Join-Path $PSScriptRoot 'make_gb_gba_roms.py') $roms | Out-Null
$env:RETRORUN_MUTE = '1'; $env:SSB64_MUTE = '1'; $env:RETROHUB_START_HINT = '0'
$cases = @(
    @{ n = 'gambatte-gb';  core = "$cores\gambatte_libretro.dll"; rom = 'gb_test.gb';   sys = 'gb';  srm = 'gb_test.srm' },
    @{ n = 'sameboy-gb';   core = "$cores\sameboy_libretro.dll";  rom = 'gb_test.gb';   sys = 'gb';  srm = 'gb_test.srm' },
    @{ n = 'sameboy_rb-gb'; core = "$bin\cores\sameboy_rb_libretro.dll"; rom = 'gb_test.gb'; sys = 'gb'; srm = 'gb_test.srm' },
    @{ n = 'mgba-gba';     core = "$cores\mgba_libretro.dll";     rom = 'gba_test.gba'; sys = 'gba'; srm = '' }
)
$fail = 0; $ran = 0
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
        $srmOk = $true
        if ($c.srm) { $f = Join-Path $sv $c.srm; $srmOk = (Test-Path $f) -and ((Get-Item $f).Length -gt 0) }
        $sum = (Select-String -Path $log -Pattern 'SYNCTEST|frames' | Select-Object -Last 2 | ForEach-Object { $_.Line }) -join ' | '
        Write-Output ("{0} {1}: exit {2} srm={3}  {4}" -f $c.n, $mode, $p.ExitCode, $srmOk, $sum)
        $ran++
        if ($p.ExitCode -ne 0 -or -not $srmOk) { $fail++ }
    }
}
if ($fail) { Write-Output "GB/GBA SYNCTEST FAILED ($fail)"; exit 1 }
if ($ran -eq 0) { Write-Output 'GB/GBA SYNCTEST SKIPPED (no cores or retrorun.exe)'; exit 77 }   # ctest SKIP_RETURN_CODE: never a green pass with 0 runs
Write-Output "GB/GBA SYNCTEST PASS ($ran runs)"
