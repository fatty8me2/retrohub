# PS1 multi-disc swap: a 2-disc homebrew .m3u, headless + muted. Checks (1) the libretro disk control swaps discs offline (--swap-disc-at),
# (2) a swap is exact under rollback: SyncTest with a swap event in the input stream (RB_SWAPDISC) re-simulates the swap frame without a mismatch.
# Exit 0 = pass, 77 = skipped (no core / python).
# Usage: powershell -File qa\ps1_discs.ps1 [-Frames 400] [-Depth 6]
param([int]$Frames = 400, [int]$Depth = 6)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$core = "$root\build\bin\Release\cores\beetle_psx_libretro.dll"
if (-not (Test-Path $core)) { $core = "$root\..\..\RetroHub\build\bin\Release\cores\beetle_psx_libretro.dll" }   # worktrees reuse the main build's core
$exe = "$root\build\bin\Release\retrorun.exe"
if (-not (Test-Path $core) -or -not (Test-Path $exe)) { Write-Output 'SKIP: beetle_psx_libretro.dll / retrorun.exe not built'; exit 77 }
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Write-Output 'SKIP: python not found'; exit 77 }
$out = Join-Path $root 'qa\out\ps1discs'
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $out, "$out\sys", "$out\sv" | Out-Null
python (Join-Path $root 'qa\make_ps1_netdisc.py') $out --name 'Test (Disc 1)' | Out-Null
python (Join-Path $root 'qa\make_ps1_netdisc.py') $out --name 'Test (Disc 2)' --interlace | Out-Null
Set-Content -Encoding ascii "$out\Test.m3u" "Test (Disc 1).cue`r`nTest (Disc 2).cue"
$env:RETRORUN_MUTE = '1'
$fail = 0
function Run($tag, $extra) {
    $log = "$out\$tag.log"
    $a = @('--core', $core, '--rom', "$out\Test.m3u", '--system', 'ps1', '--system-dir', "$out\sys", '--save-dir', "$out\sv", '--state-dir', "$out\sv",
           '--headless', '--log', $log) + $extra
    $p = Start-Process $exe -ArgumentList $a -WindowStyle Hidden -PassThru -Wait
    return @{ code = $p.ExitCode; log = $log }
}
# 1) offline swap, twice (Disc 1 -> 2 -> 1)
$r = Run 'swap' @('--frames', $Frames, '--swap-disc-at', '60,200')
$t = Get-Content $r.log -Raw
if ($r.code -ne 0) { Write-Output "FAIL swap: exit $($r.code)"; $fail++ }
elseif ($t -notmatch 'Disc 2 inserted \(frame \d+, core disc 2 of 2\)' -or $t -notmatch 'Disc 1 inserted \(frame \d+, core disc 1 of 2\)') { Write-Output 'FAIL swap: missing "Disc N inserted" lines'; $fail++ }
else { Write-Output 'PASS offline swap 1->2->1' }
# 2) rollback SyncTest with swap events
$env:RB_SWAPDISC = '90,250'
$r = Run 'synctest' @('--frames', $Frames, '--synctest', $Depth)
Remove-Item Env:RB_SWAPDISC
$t = Get-Content $r.log -Raw
if ($r.code -ne 0 -or $t -notmatch 'SYNCTEST frames.*PASS' -or $t -notmatch 'Disc 2 inserted') { Write-Output "FAIL synctest with swaps: exit $($r.code)"; Select-String $r.log -Pattern 'SYNCTEST|MISMATCH|disc' | Select-Object -First 8 | ForEach-Object { $_.Line }; $fail++ }
else { Write-Output 'PASS rollback SyncTest with disc swaps' }
if ($fail) { Write-Output "PS1 DISCS FAILED ($fail)"; exit 1 }
Write-Output 'PS1 DISCS PASS'
exit 0
