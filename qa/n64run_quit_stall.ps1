# runners-M1: the hub's Quit must be served while a netplay peer is blocked waiting for another player (it used to wait for the stall to end).
# Two headless, real-time peers on a LOCAL worker; slot 1 freezes for -StallMs at sample 600 (N64RUN_QA_STALL, RETROHUB_QA only), which blocks slot 0 in its
# lockstep wait. While slot 0 is blocked the script writes "quit" into slot 0's control file and measures how long the ack and the exit take.
# Hidden and muted; processes are stopped by PID only. Windows PowerShell 5.1.
#   powershell -File qa\n64run_quit_stall.ps1 [-Exe <n64run.exe>] [-Rom <Mario Kart 64 .z64>] [-StallMs 8000] [-MaxExitSec 3]
# PASS: ack within 1.5 s, exit code 0, exited within -MaxExitSec of the Quit although the stall lasts -StallMs.
param(
    [string]$Exe = '',
    [string]$Rom = 'C:\Users\fatty\Desktop\Everything\Games\N64\Mario Kart 64 (U) [!].z64',
    [string]$EmuDir = '',
    [int]$StallMs = 8000,
    [double]$MaxExitSec = 3.0,
    [int]$Port = 8852,
    [string]$Server = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\n64run_headless_lib.ps1"
. "$PSScriptRoot\_localworker.ps1"
if (-not $Exe) { $Exe = Join-Path $root 'build\bin\Release\n64run.exe' }
if (-not $EmuDir) { $EmuDir = Join-Path $root 'deps\emu' }
if (-not (Test-Path -LiteralPath $Rom)) { Write-Output "SKIP: no ROM at $Rom"; exit 0 }
$tmp = Join-Path $env:TEMP ('n64quit-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $tmp | Out-Null
$w = $null
$fail = 0
try {
    if ($Server -eq '') { $w = Start-LocalWorker -Port $Port -Tmp $tmp; $Server = $w.Url } else { Assert-LocalServer $Server }
    $env:RETROHUB_QA = '1'; $env:HUBNET_LOOPBACK = '1'; $env:N64RUN_QA_STALL = "600,$StallMs"
    $room = ('{0}-1' -f ([guid]::NewGuid().ToString('N').Substring(0, 6).ToUpper() -replace '[01OIL]', 'X'))
    $ctl = "$tmp\run0"; New-Item -ItemType Directory $ctl | Out-Null
    '{"seq":0}' | Set-Content "$ctl\control.json" -Encoding ascii
    $procs = @()
    foreach ($slot in 0, 1) {
        $extra = if ($slot -eq 0) { " --control `"$ctl\control.json`" --game-key qa-quit" } else { '' }
        $a = "--rom `"$Rom`" --game mk64 --emu-dir `"$EmuDir`" --save-dir `"$tmp\save$slot`" --net $Server --room $room --players 2 --ports 0,1,-1,-1 --slot $slot --headless --realtime --frames 0 --test-input random:3$extra"
        $procs += @{ P = (Start-HeadlessRun $Exe $a "$tmp\peer$slot"); Base = "$tmp\peer$slot" }
    }
    # wait until the stall has started: slot 0 logs its sync check of sample 600 right before slot 1 freezes
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $seen = $false
    while ($sw.Elapsed.TotalSeconds -lt 90 -and -not $seen) {
        Start-Sleep -Milliseconds 100
        $log = Get-Content "$tmp\peer0.err.txt" -ErrorAction SilentlyContinue
        if ($log -and ($log -match 'sync check sample 600 ')) { $seen = $true }
        if ($procs[0].P.HasExited) { break }
    }
    if (-not $seen) { Write-Output 'FAIL: slot 0 never reached sample 600'; $fail++ }
    else {
        Start-Sleep -Milliseconds 1500   # well inside the stall
        $t0 = Get-Date
        [IO.File]::WriteAllText("$ctl\control.json", '{"seq":1,"cmd":"quit","slot":0,"value":0,"speed":1,"text":""}')
        $ack = $null
        while (((Get-Date) - $t0).TotalSeconds -lt 10 -and -not $ack) {
            if (Test-Path "$ctl\control_ack.json") { try { $j = Get-Content "$ctl\control_ack.json" -Raw | ConvertFrom-Json; if ($j.seq -eq 1) { $ack = $j } } catch {} }
            Start-Sleep -Milliseconds 50
        }
        $ackSec = ((Get-Date) - $t0).TotalSeconds
        $null = $procs[0].P.WaitForExit(30000)
        $exitSec = ((Get-Date) - $t0).TotalSeconds
        $code = if ($procs[0].P.HasExited) { $procs[0].P.ExitCode } else { -1 }
        Write-Output ('quit sent {0:N1} s into a {1} ms stall: ack after {2:N2} s, exit after {3:N2} s, exit code {4}' -f 1.5, $StallMs, $ackSec, $exitSec, $code)
        if (-not $ack -or $ackSec -gt 1.5) { Write-Output 'FAIL: no ack within 1.5 s'; $fail++ }
        if ($code -ne 0) { Write-Output "FAIL: exit code $code (want 0)"; $fail++ }
        if ($exitSec -gt $MaxExitSec) { Write-Output ("FAIL: exited {0:N1} s after Quit (limit {1})" -f $exitSec, $MaxExitSec); $fail++ }
        if (-not (Select-String -Path "$tmp\peer0.err.txt" -Pattern 'emulation blocked' -Quiet)) { Write-Output 'note: the Quit was not served from a blocked wait (the stall may have ended first)' }
    }
} finally {
    foreach ($pr in $procs) { try { if (-not $pr.P.HasExited) { $pr.P.Kill() } } catch {} }
    Stop-LocalWorker $w
    foreach ($s in 0, 1) { if (Test-Path "$tmp\peer$s.err.txt") { Copy-Item "$tmp\peer$s.err.txt" (Join-Path $env:TEMP "n64quit-last-peer$s.err.txt") -Force } }
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($fail) { Write-Output "FAILED ($fail)"; exit 1 }
Write-Output 'PASS'
exit 0
