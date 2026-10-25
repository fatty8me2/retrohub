# Headless N64 host-leaves-mid-game QA (Windows PowerShell 5.1): N n64run peers on a LOCAL worker, the host (slot 0) is killed mid-match.
# Survivors must end cleanly with exit 8 ("a player left, game ended") and never hang or desync.
#   powershell -File qa\n64run_hostkill.ps1 -Server ws://127.0.0.1:9400 -Rom "...\Mario Kart 64 (U) [!].z64" -Players 3
param(
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Rom = '',
    [int]$Players = 2,
    [int]$KillSlot = 0,
    [int]$KillAfterSec = 20,
    [int]$TimeoutSec = 120,
    [switch]$ExpectDrop   # kill a GUEST (-KillSlot 1+, -Players 3+): the survivors must keep playing (alive 15 s later, drop logged, sync-check hashes identical), then are stopped
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
    if ($ExpectDrop -and $killed -and $sw.Elapsed.TotalSeconds -ge $KillAfterSec + 15) { break }
    $alive = @($procs.Keys | Where-Object { $_ -ne $KillSlot -and -not $procs[$_].HasExited })
    if ($killed -and $alive.Count -eq 0) { break }
    Start-Sleep -Milliseconds 200
}
if ($ExpectDrop) {
    $fail = 0; $hs = @{}
    foreach ($slot in 0..($Players - 1)) {
        if ($slot -eq $KillSlot) { continue }
        $p = $procs[$slot]
        if ($p.HasExited) { Write-Output "slot $slot exited early (code $($p.ExitCode))"; $fail++; continue }
        try { $p.Kill() } catch {}   # our own PID
        $p.WaitForExit()
        $err = Get-Content "$tmp\peer$slot.err.txt" -ErrorAction SilentlyContinue
        $txt = $err | Out-String
        $dropped = ([regex]::Matches($txt, 'disconnected, neutral from')).Count
        $desync = ([regex]::Matches($txt, 'DESYNC')).Count
        $m = @{}; foreach ($mm in [regex]::Matches($txt, 'sync check sample (\d+) \S+ ([0-9a-f]{16})')) { $m[[int]$mm.Groups[1].Value] = $mm.Groups[2].Value }
        $hs[$slot] = $m
        Write-Output ("slot $slot drop-lines=$dropped desync=$desync sync-checks=" + $m.Count)
        if (-not $dropped -or $desync) { $fail++ }
    }
    $slots = @($hs.Keys); $common = 0
    if ($slots.Count -ge 2) { foreach ($k in $hs[$slots[0]].Keys) { if ($hs[$slots[1]].ContainsKey($k)) { $common++; if ($hs[$slots[1]][$k] -ne $hs[$slots[0]][$k]) { Write-Output "HASH MISMATCH at sample $k"; $fail++ } } } }
    Write-Output "common sync checks=$common"
    if ($common -lt 2) { $fail++ }
    foreach ($slot in 0..($Players - 1)) { if ($slot -ne $KillSlot) { } }
    if ($fail) { Write-Output "RESULT: FAIL"; exit 1 }
    Write-Output "RESULT: PASS"; exit 0
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
