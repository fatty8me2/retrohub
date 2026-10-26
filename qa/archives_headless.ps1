# Headless archive launch checks (no window, muted, offline, scratch dirs only):
#   GBA from .zip, NES + SNES from .7z, N64 from .7z (key + byte-identical unpack; n64run cannot run headless), PS1 cue/bin from .zip
# The library lists each archive, unpacks it into a scratch rom-cache, and retrorun --headless runs the unpacked file for a few frames.
# Exit 0 pass, 1 fail, 77 skipped. Usage: powershell -NoProfile -File qa\archives_headless.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\bin\Release'
$bench = Join-Path $bin 'scan_bench.exe'; $rr = Join-Path $bin 'retrorun.exe'
$cores = Join-Path $root 'deps\cores'; $roms = Join-Path $root 'deps\testroms'
if (-not (Test-Path $bench) -or -not (Test-Path $rr) -or -not (Test-Path "$roms\sram.gba")) { Write-Output 'SKIP: scan_bench / retrorun / test roms missing'; exit 77 }
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$w = Join-Path $env:TEMP 'archives_headless'
Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
$lib = Join-Path $w 'lib'; $data = Join-Path $w 'data'; $sv = Join-Path $w 'sv'
New-Item -ItemType Directory -Force -Path $lib, $data, $sv | Out-Null
$env:RETRORUN_MUTE = '1'; $env:SSB64_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:RETROHUB_START_HINT = '0'
function Pack($fmt, $dir, $file, $dest) { & $tar -a -cf $dest -C $dir $file; if ($LASTEXITCODE) { throw "tar failed for $dest" } }
$g = Join-Path $w 'gbasrc'; New-Item -ItemType Directory $g | Out-Null   # the library ignores ROMs under 16 KB: pad the 2 KB homebrew test ROM
$b = [IO.File]::ReadAllBytes("$roms\sram.gba"); $pad = New-Object byte[] 131072; [Array]::Copy($b, $pad, $b.Length); [IO.File]::WriteAllBytes("$g\sram.gba", $pad)
Pack 'zip' $g 'sram.gba' "$lib\gba game.zip"
Pack '7z' $roms 'nes_test.nes' "$lib\nes game.7z"
Pack '7z' $roms 'snes_test.sfc' "$lib\snes game.7z"
$n64src = Get-ChildItem -Path "$env:USERPROFILE\Desktop\Everything\Games\N64" -Filter *.z64 -ErrorAction SilentlyContinue | Sort-Object Length | Select-Object -First 1
if ($n64src) { $n = Join-Path $w 'n64src'; New-Item -ItemType Directory $n | Out-Null; Copy-Item -LiteralPath $n64src.FullName -Destination "$n\rom.z64"; Pack '7z' $n 'rom.z64' "$lib\n64 game.7z" }
$ps = Join-Path $w 'ps1'; New-Item -ItemType Directory $ps | Out-Null
$havePs = $false
if (Get-Command python -ErrorAction SilentlyContinue) {
    python (Join-Path $root 'qa\make_ps1_netdisc.py') $ps --name 'Test' | Out-Null
    if (Test-Path "$ps\Test.cue") { $havePs = $true; $names = @(Get-ChildItem $ps -File | ForEach-Object Name); & $tar -a -cf "$lib\my backup.zip" -C $ps @names }   # not a Redump name: identified from SYSTEM.CNF
}
$env:SCAN_BENCH_LAUNCH = '1'
$out = & $bench $data $lib 2>&1
$out | Out-String | Write-Output
$fail = 0
$launch = @{}
foreach ($l in $out) { if ("$l" -match '^launch (\S+) (\S+) (.+)$') { $launch[$Matches[1]] = $Matches[3] } }
function Run($sys, $core, $rom, $extra) {
    $log = Join-Path $w "$sys.log"
    $a = @('--core', $core, '--rom', $rom, '--system', $sys, '--save-dir', $sv, '--state-dir', $sv, '--system-dir', $sv, '--headless', '--frames', '120', '--log', $log) + $extra
    $p = Start-Process -FilePath $rr -ArgumentList $a -PassThru -WindowStyle Hidden -Wait
    return $p.ExitCode
}
$cases = @(@('gba', "$cores\mgba_libretro.dll"), @('nes', "$cores\nestopia_libretro.dll"), @('snes', "$cores\snes9x_libretro.dll"))
if ($havePs) { $cases += ,@('ps1', "$bin\cores\beetle_psx_libretro.dll") }
foreach ($c in $cases) {
    $rom = $launch[$c[0]]
    if (-not $rom -or -not (Test-Path $rom)) { Write-Output "FAIL $($c[0]): no unpacked launch path"; $fail++; continue }
    if (-not (Test-Path $c[1])) { Write-Output "SKIP $($c[0]): core missing"; continue }
    $code = Run $c[0] $c[1] $rom @()
    if ($code -ne 0) { Write-Output "FAIL $($c[0]): retrorun exit $code ($rom)"; $fail++ } else { Write-Output "ok   $($c[0]) from archive: $rom" }
}
if ($n64src) {
    $rom = $launch['n64']
    if (-not $rom -or -not (Test-Path $rom)) { Write-Output 'FAIL n64: no unpacked launch path'; $fail++ }
    elseif ((Get-FileHash -LiteralPath $rom).Hash -ne (Get-FileHash -LiteralPath $n64src.FullName).Hash) { Write-Output 'FAIL n64: unpacked bytes differ'; $fail++ }
    else { Write-Output "ok   n64 from .7z: byte-identical unpack $rom" }
}
if ($fail) { exit 1 }
Write-Output 'archives_headless: ok'
