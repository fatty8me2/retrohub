# Headless N64 host-leaves-mid-game QA (Windows PowerShell 5.1): N n64run peers on a LOCAL worker, the host (slot 0) is killed mid-match.
# Survivors must end cleanly with exit 8 ("a player left, game ended") and never hang or desync.
#   powershell -File qa\n64run_hostkill.ps1 -Server ws://127.0.0.1:9400 -Rom "...\Mario Kart 64 (U) [!].z64" -Players 3
param(
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Rom = '',
    [int]$Players = 2,
    [int]$KillSlot = 0,
    [int]$KillAfterSec = 20,
    [int]$TimeoutSec = 120
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
. "$PSScriptRoot\n64run_headless_lib.ps1"
$root = Split-Path -Parent $PSScriptRoot
$n64 = Join-Path $root 'build\bin\Release\n64run.exe'
if (-not (Test-Path -LiteralPath $Rom)) { Write-Output "SKIP: pass -Rom"; exit 0 }
$tmp = Join-Path $root ('qa\out\n64hk' + $Players + 'k' + $KillSlot)
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force $tmp | Out-Null
$room = ('{0}-1' -f ([guid]::NewGuid().ToString('N').Substring(0, 6).ToUpper() -replace '[01OIL]', 'X'))
$ports = (0..3 | ForEach-Object { if ($_ -lt $Players) { "$_" } else { '-1' } }) -join ','
$procs = @{}
foreach ($slot in 0..($Players - 1)) {
    $a = "--rom `"$Rom`" --game mk64 --save-dir `"$tmp\save$slot`" --net $Server --room $room --players $Players --ports $ports --slot $slot --headless"
    $procs[$slot] = Start-HeadlessRun $n64 $a "$tmp\peer$slot"
}
$sw = [Diagnostics.Stopwatch]::StartNew()
$killed = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    if (-not $killed -and $sw.Elapsed.TotalSeconds -ge $KillAfterSec) {
        try { $procs[$KillSlot].Kill() } catch {}   # our own PID only
        $killed = $true
        Write-Output ("killed slot $KillSlot at " + [int]$sw.Elapsed.TotalSeconds + "s")
    }
    $alive = @($procs.Keys | Where-Object { $_ -ne $KillSlot -and -not $procs[$_].HasExited })
    if ($killed -and $alive.Count -eq 0) { break }
    Start-Sleep -Milliseconds 200
}
$want = if ($KillSlot -eq 0) { @(8) } else { @(4, 8) }
$fail = 0
foreach ($slot in 0..($Players - 1)) {
    if ($slot -eq $KillSlot) { continue }
    $p = $procs[$slot]
    if (-not $p.HasExited) { try { $p.Kill() } catch {}; Write-Output "slot $slot HUNG"; $fail++; continue }
    $p.WaitForExit()
    $err = Get-Content "$tmp\peer$slot.err.txt" -ErrorAction SilentlyContinue | Out-String
    $desync = ([regex]::Matches($err, 'DESYNC')).Count
    Write-Output ("slot $slot exit=" + $p.ExitCode + " desync=" + $desync)
    if ($want -notcontains $p.ExitCode -or $desync) { $fail++ }
}
if ($fail) { Write-Output "RESULT: FAIL"; exit 1 }
Write-Output "RESULT: PASS"
exit 0
