# Headless cheat check (no window, muted): run mGBA on a test ROM with a RAM-write cheat and read the memory back
# with --dump-mem; then flip the cheat off/on in the file and verify live toggling. Usage: qa\cheats_headless.ps1 [-Root <worktree>]
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
$exe = Join-Path $Root 'build\bin\Release\retrorun.exe'
$core = Join-Path $Root 'deps\cores\mgba_libretro.dll'
$rom = Join-Path $Root 'deps\testroms\arm.gba'
$work = Join-Path $env:TEMP ('cheats_qa_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$work\sv", "$work\st" | Out-Null
$cj = "$work\cheats.json"; $dump = "$work\mem.bin"
function Run($frames) {
    Remove-Item $dump -ErrorAction SilentlyContinue
    & $exe --core $core --rom $rom --system gba --save-dir "$work\sv" --state-dir "$work\st" --headless --frames $frames --cheats $cj --dump-mem "2,16,8,$dump" 2>&1 | ForEach-Object { if ($_ -match 'cheats') { Write-Host $_ } }
    if (Test-Path $dump) { return ([BitConverter]::ToString([IO.File]::ReadAllBytes($dump))) } else { return 'NODUMP' }
}
$fail = 0
# GameShark v3 32-bit write 2xxxxxxx: EWRAM 0x02000010 <- DEADBEEF (little endian in memory)
'{"version":1,"cheats":[{"id":"t1","desc":"write","format":"ar","codes":["22000010 DEADBEEF"],"enabled":true}]}' | Set-Content $cj -Encoding ascii
$on = Run 120
$cj2 = '{"version":1,"cheats":[{"id":"t1","desc":"write","format":"ar","codes":["22000010 DEADBEEF"],"enabled":false}]}'
$cj2 | Set-Content $cj -Encoding ascii
$off = Run 120
Write-Host "enabled : $on"; Write-Host "disabled: $off"
if ($on -ne 'EF-BE-AD-DE-00-00-00-00') { Write-Host 'FAIL: cheat did not write EWRAM'; $fail++ }
if ($off -eq $on -or $off -ne '00-00-00-00-00-00-00-00') { Write-Host 'FAIL: disabled cheat still wrote'; $fail++ }
# Live toggle: start disabled, long run, enable mid-run (file rewrite), expect the write at exit.
$cj2 | Set-Content $cj -Encoding ascii
Remove-Item $dump -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -ArgumentList @('--core', $core, '--rom', $rom, '--system', 'gba', '--save-dir', "$work\sv", '--state-dir', "$work\st", '--headless', '--frames', '12000', '--cheats', $cj, '--dump-mem', "2,16,8,$dump", '--log', "$work\live.log")
Start-Sleep -Milliseconds 1500
'{"version":1,"cheats":[{"id":"t1","desc":"write","format":"ar","codes":["22000010 DEADBEEF"],"enabled":true}]}' | Set-Content $cj -Encoding ascii
if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id; Write-Host 'FAIL: live run timed out'; $fail++ }
$live = if (Test-Path $dump) { [BitConverter]::ToString([IO.File]::ReadAllBytes($dump)) } else { 'NODUMP' }
Write-Host "live    : $live"
if ($live -ne 'EF-BE-AD-DE-00-00-00-00') { Write-Host 'FAIL: live enable did not take effect (run may have finished before the edit)'; $fail++ }
Remove-Item -Recurse -Force $work
if ($fail) { exit 1 } else { Write-Host 'cheats_headless: ok' }
