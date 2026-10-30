# Pack install end to end, offline: fake Drive (qa\gamepack_fake_server.py --packdir) serving fake archives (qa\make_pack_fixture.py),
# real Installer -> scan of <data>\gamepack -> ghost check -> unpack for Play. Headless, muted, no windows, local server only.
# Exit 0 pass, 1 fail, 77 skipped. Usage: powershell -NoProfile -File qa\pack_install_e2e.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\qa_gamepack.exe'
if (-not (Test-Path $exe) -or -not (Get-Command python -ErrorAction SilentlyContinue)) { Write-Output 'SKIP: qa_gamepack.exe or python missing'; exit 77 }
$w = Join-Path $env:TEMP 'pack_install_e2e'
Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$w\bin" | Out-Null
Copy-Item $exe "$w\bin\qa_gamepack.exe"
python (Join-Path $root 'qa\make_pack_fixture.py') "$w\pack" | Out-Null
$port = 18771
$srv = Start-Process python -ArgumentList @((Join-Path $root 'qa\gamepack_fake_server.py'), $port, '--packdir', "$w\pack") -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$env:RETROHUB_QA = '1'; $env:RETROHUB_GP_BASE = "127.0.0.1:$port"; $env:SSB64_MUTE = '1'; $env:RETROHUB_NO_SHELL = '1'
$cases = @(@('gc', 'Gauntlet - Dark Legacy'), @('gc', 'Fakeland Zip'), @('gc', 'Pikmin'), @('gc', 'Generic Inner Game'), @('gc', 'Folder Style Game'),
           @('ps2', 'Gauntlet - Dark Legacy'), @('ps1', 'Crash Bandicoot'), @('wiiu', 'Fakeland WiiU'), @('gba', 'Control GBA Game'))
$fail = 0
try {
    foreach ($c in $cases) {
        Remove-Item -Recurse -Force "$w\bin\userdata" -ErrorAction SilentlyContinue
        $out = (& "$w\bin\qa_gamepack.exe" e2e '-' $c[0] $c[1] 2>&1 | Out-String)
        $okc = ($out -match 'ghost_before=1') -and ($out -match 'install ok=1') -and ($out -match 'scan_found=1') -and ($out -match 'ghost_after=0') -and -not ($out -match 'unpack ok=0')
        Write-Output ("{0} {1} / {2}" -f $(if ($okc) { 'PASS' } else { 'FAIL' }), $c[0], $c[1])
        if (-not $okc) { $fail++; Write-Output $out }
    }
} finally { $srv.Kill() }
exit $(if ($fail) { 1 } else { 0 })
