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
    # Gambatte keeps the MBC3 clock on wall time (not rollback-safe by design: online play never uses Gambatte for a clock cartridge),
    # so its SyncTest uses the clock-free ROM; the clock ROM is checked by the --net refusal below instead.
    @{ n = 'gambatte-gb';  core = "$cores\gambatte_libretro.dll"; rom = 'gb_test_nortc.gb'; sys = 'gb';  srm = 'gb_test_nortc.srm' },
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
# Gambatte + a clock cartridge must be refused online (exit 12, plain reason) BEFORE connecting; a clock-free ROM must not be refused for that reason.
$gam = "$cores\gambatte_libretro.dll"
if ((Test-Path $gam) -and (Test-Path $exe)) {
    foreach ($t in @(@{ rom = 'gb_test.gb'; refuse = $true }, @{ rom = 'gb_test_nortc.gb'; refuse = $false })) {
        $reason = Join-Path $out ("refuse_" + $t.rom + ".txt"); $log = Join-Path $out ("refuse_" + $t.rom + ".log")
        Remove-Item -ErrorAction SilentlyContinue $reason, $log
        $sv = Join-Path $out 'sv_refuse'; New-Item -ItemType Directory -Force -Path $sv | Out-Null
        $env:RETRORUN_REASON_FILE = $reason
        $a = @('--core', $gam, '--rom', (Join-Path $roms $t.rom), '--system', 'gb', '--save-dir', $sv, '--state-dir', $sv, '--net', 'ws://127.0.0.1:9',
               '--room', 'X-1', '--slot', '0', '--players', '2', '--headless', '--frames', 60, '--log', $log)
        $p = Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(30000)) { taskkill /PID $p.Id /T /F | Out-Null }
        $txt = if (Test-Path $reason) { Get-Content -Raw $reason } else { '' }
        $refused = ($p.ExitCode -eq 12) -and ($txt -match 'keeps its own clock')
        $ok = ($refused -eq $t.refuse)
        Write-Output ("gambatte-net {0}: exit {1} refused-for-clock={2} expected={3} {4}" -f $t.rom, $p.ExitCode, $refused, $t.refuse, $(if ($ok) { 'ok' } else { 'WRONG' }))
        $ran++
        if (-not $ok) { $fail++ }
    }
    Remove-Item Env:\RETRORUN_REASON_FILE -ErrorAction SilentlyContinue
}
if ($fail) { Write-Output "GB/GBA SYNCTEST FAILED ($fail)"; exit 1 }
if ($ran -eq 0) { Write-Output 'GB/GBA SYNCTEST SKIPPED (no cores or retrorun.exe)'; exit 77 }   # ctest SKIP_RETURN_CODE: never a green pass with 0 runs
Write-Output "GB/GBA SYNCTEST PASS ($ran runs)"
