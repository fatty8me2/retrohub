# PS1 (Beetle PSX) rollback SyncTest on license-free homebrew discs (OpenBIOS). Headless, muted, offline.
# Every frame: roll back -Depth frames, re-simulate, compare the picture + RAM with the free-running reference (mispredicted frames included);
# the homebrew (qa\make_ps1_netdisc.py) polls the DualShock pads through SIO0, so sticks and buttons are part of the saved state.
# Runs a 240p disc (N) and a 480i disc (I) unless -Rom is given. Exit 0 = pass, 77 = skipped (no core / no python).
# Usage: powershell -File qa\ps1_synctest.ps1 [-Frames 700] [-Depth 6] [-Rom <cue>] [-Extra '--opt,k=v']
param([int]$Frames = 700, [int]$Depth = 6, [string]$Rom = '', [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$core = "$root\build\bin\Release\cores\beetle_psx_libretro.dll"
$exe = "$root\build\bin\Release\retrorun.exe"
if (-not (Test-Path $core) -or -not (Test-Path $exe)) { Write-Output 'SKIP: beetle_psx_libretro.dll / retrorun.exe not built'; exit 77 }
$out = Join-Path $root 'qa\out\ps1'
New-Item -ItemType Directory -Force -Path $out, "$out\sys", "$out\sv" | Out-Null
$discs = @()
if ($Rom) { $discs = @($Rom) }
else {
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Write-Output 'SKIP: python not found (needed to build the test discs)'; exit 77 }
    python (Join-Path $root 'qa\make_ps1_netdisc.py') $out --name N | Out-Null
    python (Join-Path $root 'qa\make_ps1_netdisc.py') $out --name I --interlace | Out-Null
    $discs = @("$out\N.cue", "$out\I.cue")
}
$env:RETRORUN_MUTE = '1'
Remove-Item Env:RB_SAVEFRAMES, Env:RB_SAVEBYHASH -ErrorAction SilentlyContinue
$fail = 0
foreach ($d in $discs) {
    $log = "$out\syn_" + [IO.Path]::GetFileNameWithoutExtension($d) + '.log'
    $a = @('--core', $core, '--rom', $d, '--system', 'ps1', '--system-dir', "$out\sys", '--save-dir', "$out\sv", '--state-dir', "$out\sv",
           '--headless', '--log', $log, '--frames', $Frames, '--synctest', $Depth) + @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    $p = Start-Process $exe -ArgumentList $a -WindowStyle Hidden -PassThru -Wait
    $sum = Select-String $log -Pattern 'SYNCTEST frames|MISMATCH' | Select-Object -First 4 | ForEach-Object { $_.Line }
    Write-Output ("{0}: exit {1}`n   {2}" -f [IO.Path]::GetFileName($d), $p.ExitCode, ($sum -join "`n   "))
    if ($p.ExitCode -ne 0) { $fail++ }
}
if ($fail) { Write-Output "PS1 SYNCTEST FAILED ($fail)"; exit 1 }
Write-Output 'PS1 SYNCTEST PASS'
exit 0
