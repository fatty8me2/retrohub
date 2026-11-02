# Pack install end to end, offline: fake Drive (qa\gamepack_fake_server.py --packdir) serving fake archives (qa\make_pack_fixture.py),
# real Installer (sizes up front -> staging download -> one rename -> single-folder scan -> unpack for first Play) -> ghost check ->
# uninstall (ghost comes back). Also Drive's big-file "can't scan for viruses" form, a cut connection (resume) and the quota page.
# Headless, muted, no windows, local server only. Exit 0 pass, 1 fail, 77 skipped. Usage: powershell -NoProfile -File qa\pack_install_e2e.ps1
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
           @('ps2', 'Gauntlet - Dark Legacy'), @('ps1', 'Crash Bandicoot'), @('wiiu', 'Fakeland WiiU'), @('gba', 'Control GBA Game'), @('n64', 'Fakeland 64'), @('n64', 'Zipland Cart'), @('nds', 'Fakeland DS'))
$fail = 0
function Mode($m) { Invoke-WebRequest "http://127.0.0.1:$port/__mode?dl=$m" -UseBasicParsing | Out-Null }
function RunCase($c) {
    Remove-Item -Recurse -Force "$w\bin\userdata" -ErrorAction SilentlyContinue
    return (& "$w\bin\qa_gamepack.exe" e2e '-' $c[0] $c[1] 2>&1 | Out-String)
}
try {
    foreach ($c in $cases) {
        Mode 'ok'
        $out = RunCase $c
        $isArchive = $out -match 'issue=extract'
        $okc = ($out -match 'ghost_before=1') -and ($out -match 'install ok=1') -and ($out -match 'total_known=1') -and ($out -match 'staging_left=0') `
               -and ($out -match 'scan_found=1') -and ($out -match 'ghost_after=0') -and ($out -match 'full_scan_same_keys=1') `
               -and ($out -match 'uninstalled=1 dir_gone=1 installed_flag=0 ghost_back=1') -and (-not $isArchive -or ($out -match 'prepared=1')) -and -not ($out -match 'prepared=0') `
               -and ($out -match 'play_ok=1') -and -not ($out -match 'real_image=0|full_scan_same_path=0')   # Play right after the install, same process: a real image path
        $tag = ''
        if ($isArchive) { $tag = ' (archive unpacked at install)' }
        Write-Output ("{0} {1} / {2}{3}" -f $(if ($okc) { 'PASS' } else { 'FAIL' }), $c[0], $c[1], $tag)
        if (-not $okc) { $fail++; Write-Output $out }
    }
    # Drive's big-file "can't scan for viruses" form (needs the uuid), and a connection cut half way (continues from the .part)
    foreach ($m in @('confirm', 'drop')) {
        Mode $m
        $out = RunCase @('gc', 'Fakeland Zip')
        $okc = ($out -match 'install ok=1') -and ($out -match 'prepared=1') -and ($out -match 'ghost_after=0')
        Write-Output ("{0} drive mode {1}" -f $(if ($okc) { 'PASS' } else { 'FAIL' }), $m)
        if (-not $okc) { $fail++; Write-Output $out }
    }
    # PS2 CDs (cue + bin in a 7z) filed under the GameCube folder: kept, listed as PlayStation 2, the GameCube ghost is cleared, unpacked for Play
    foreach ($t in @('Gauntlet - Seven Sorrows', 'Fakeland CD')) {
        Mode 'ok'
        $out = RunCase @('gc', $t)
        $okc = ($out -match 'ghost_before=1') -and ($out -match 'install ok=1') -and ($out -match 'entry sys=ps2 ') -and ($out -match 'prepared=1') -and ($out -match 'ghost_after=0') `
               -and ($out -match 'PlayStation 2 game \(the pack had it under GameCube\)') -and ($out -match 'full_scan_same_keys=1') -and ($out -match 'play_ok=1') -and ($out -match 'uninstalled=1 dir_gone=1 installed_flag=0 ghost_back=1')
        Write-Output ("{0} misfiled PS2 cue/bin in gc folder: {1}" -f $(if ($okc) { 'PASS' } else { 'FAIL' }), $t)
        if (-not $okc) { $fail++; Write-Output $out }
    }
    # a file that is no game at all: the install fails with a plain message, nothing stays on disk, no finished marker, the ghost stays
    Mode 'ok'
    $out = RunCase @('gc', 'Unplayable Thing')
    $okc = ($out -match 'install ok=0') -and ($out -match 'is not a game RetroHub can play') -and ($out -match 'dir_exists=0 done_marker=0 staging_left=0') -and ($out -match 'ghost_after=1')
    Write-Output ("{0} unidentifiable file fails cleanly" -f $(if ($okc) { 'PASS' } else { 'FAIL' }))
    if (-not $okc) { $fail++; Write-Output $out }
    # quota page: plain-English failure, ghost stays
    Mode 'quota'
    $out = RunCase @('gc', 'Fakeland Zip')
    $okc = ($out -match 'install ok=0') -and ($out -match 'friendly=\[The shared pack is busy') -and ($out -match 'ghost_after=1')
    Write-Output ("{0} drive mode quota (plain message)" -f $(if ($okc) { 'PASS' } else { 'FAIL' }))
    if (-not $okc) { $fail++; Write-Output $out }
    Mode 'ok'
} finally { $srv.Kill() }
exit $(if ($fail) { 1 } else { 0 })
