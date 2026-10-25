# Headless N64 cheat check (hidden window, muted, ROM read-only): a GameShark 16-bit write into unused top-of-RDRAM,
# read back with --dump-ram; then a live enable mid-run through the polled cheats file.
param([string]$Root = (Split-Path -Parent $PSScriptRoot),
      [string]$Rom = 'C:\Users\fatty\Desktop\Everything\Games\N64\Mario Kart 64 (U) [!].z64',
      [string]$EmuDir = '')
$ErrorActionPreference = 'Stop'
$env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
if (-not (Test-Path -LiteralPath $Rom)) { Write-Host "SKIP: no ROM at $Rom"; exit 0 }
if (-not $EmuDir) { $EmuDir = Join-Path $Root 'deps\emu' }
$exe = Join-Path $Root 'build\bin\Release\n64run.exe'
$work = Join-Path $env:TEMP ('cheats_n64_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
$cj = "$work\cheats.json"; $dump = "$work\ram.bin"
$on  = '{"version":1,"cheats":[{"id":"t1","desc":"write","format":"gameshark","codes":["817F0000 BEEF"],"enabled":true},{"id":"bad","desc":"unsupported","format":"gameshark","codes":["DE000000 0000"],"enabled":true}]}'
$off = $on -replace '"enabled":true},\{"id":"bad"', '"enabled":false},{"id":"bad"'
function Args2($frames) { @('--rom', "`"$Rom`"",'--game', 'qa-cheat', '--headless', '--frames', $frames, '--emu-dir', $EmuDir, '--save-dir', "$work\save", '--cheats', $cj, '--dump-ram', "0x7F0000,4,$dump") }
function Run($frames) {
    Remove-Item $dump -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -ArgumentList (Args2 $frames) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\o.txt" -RedirectStandardError "$work\e.txt"
    if (-not $p.WaitForExit(180000)) { Stop-Process -Id $p.Id; return 'TIMEOUT' }
    Get-Content "$work\e.txt" | Where-Object { $_ -match '\[cheats\]' } | ForEach-Object { Write-Host $_ }
    if (Test-Path $dump) { return [BitConverter]::ToString([IO.File]::ReadAllBytes($dump)) } else { return 'NODUMP' }
}
$fail = 0
$on | Set-Content $cj -Encoding ascii;  $r1 = Run 300
$off | Set-Content $cj -Encoding ascii; $r2 = Run 300
Write-Host "enabled : $r1"; Write-Host "disabled: $r2"
if ($r1 -ne 'BE-EF-00-00') { Write-Host 'FAIL: enabled cheat did not write'; $fail++ }
if ($r2 -ne '00-00-00-00') { Write-Host "FAIL: disabled run not clean ($r2)"; $fail++ }
# live: start disabled, enable after ~4 s
$off | Set-Content $cj -Encoding ascii
Remove-Item $dump -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList (Args2 6000) -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\o2.txt" -RedirectStandardError "$work\e2.txt"
Start-Sleep -Seconds 6
$on | Set-Content $cj -Encoding ascii
if (-not $p.WaitForExit(240000)) { Stop-Process -Id $p.Id; Write-Host 'FAIL: live run timed out'; $fail++ }
$live = if (Test-Path $dump) { [BitConverter]::ToString([IO.File]::ReadAllBytes($dump)) } else { 'NODUMP' }
Write-Host "live    : $live"
if ($live -ne 'BE-EF-00-00') { Write-Host 'FAIL: live enable did not take effect'; $fail++ }
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if ($fail) { exit 1 } else { Write-Host 'cheats_n64_headless: ok' }
