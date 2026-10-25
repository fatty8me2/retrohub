# Headless PS1 cheat check (no window, muted): Beetle PSX on the generated test disc with a GameShark 16-bit RAM write cheat,
# memory read back with --dump-mem (RAM id 2). Enabled -> value written; disabled -> untouched.
# Exit 0 pass, 1 fail, 77 skipped. Usage: powershell -File qa\cheats_ps1_headless.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$core = "$root\build\bin\Release\cores\beetle_psx_libretro.dll"
if (-not (Test-Path $core)) { $core = "$root\..\..\RetroHub\build\bin\Release\cores\beetle_psx_libretro.dll" }
$exe = "$root\build\bin\Release\retrorun.exe"
if (-not (Test-Path $core) -or -not (Test-Path $exe) -or -not (Get-Command python -ErrorAction SilentlyContinue)) { Write-Output 'SKIP: core / retrorun / python missing'; exit 77 }
$out = Join-Path $root 'qa\out\cheats_ps1'
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $out, "$out\sys", "$out\sv" | Out-Null
python (Join-Path $root 'qa\make_ps1_netdisc.py') $out --name 'Test' | Out-Null
$env:RETRORUN_MUTE = '1'
function Run($enabled) {
    $cj = "$out\cheats.json"; $dump = "$out\mem.bin"
    Remove-Item $dump -ErrorAction SilentlyContinue
    $en = if ($enabled) { 'true' } else { 'false' }
    "{`"version`":1,`"cheats`":[{`"id`":`"p1`",`"desc`":`"write`",`"format`":`"gs`",`"codes`":[`"801F0010 BEEF`"],`"enabled`":$en}]}" | Set-Content $cj -Encoding ascii
    $a = @('--core', $core, '--rom', "$out\Test.cue", '--system', 'ps1', '--system-dir', "$out\sys", '--save-dir', "$out\sv", '--state-dir', "$out\sv",
           '--headless', '--frames', '300', '--cheats', $cj, '--dump-mem', "2,2031632,4,$dump", '--log', "$out\run$enabled.log")
    Start-Process $exe -ArgumentList $a -WindowStyle Hidden -Wait | Out-Null
    if (Test-Path $dump) { return [BitConverter]::ToString([IO.File]::ReadAllBytes($dump)) } else { return 'NODUMP' }
}
$on = Run $true; $off = Run $false
Write-Output "enabled : $on"; Write-Output "disabled: $off"
$fail = 0
if ($on -ne 'EF-BE-00-00') { Write-Output 'FAIL: PS1 cheat did not write RAM'; $fail++ }
if ($off -eq 'NODUMP' -or $off -eq $on) { Write-Output 'FAIL: disabled cheat wrote (or no dump)'; $fail++ }
if ($fail) { Get-Content "$out\runTrue.log" | Select-String cheat | Select-Object -First 6 | ForEach-Object { $_.Line }; exit 1 }
Write-Output 'cheats_ps1_headless: ok'
