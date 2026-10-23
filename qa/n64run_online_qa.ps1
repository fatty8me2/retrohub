# Headless QA for N64 online play (n64run lockstep + host-save sync + hub launch arguments).
# Windows PowerShell 5.1. Opens no windows: console tests, argument rejections, and n64run --headless runs
# (hidden SDL window, no audio device, unlimited speed; the script asserts no visible top-level window).
# The headless runs need the emulator in deps\emu and a Mario Kart 64 ROM (-Rom); they are skipped if absent.
#
# Usage:
#   powershell -File qa\n64run_online_qa.ps1                 (offline-safe checks only)
#   powershell -File qa\n64run_online_qa.ps1 -Server wss://smash-netplay.jumpcircuit.workers.dev
#   $env:HUBNET_FORCE_RELAY=1 ... (forces the WebSocket relay path for the live tests)
param(
    [string]$Server = '',
    [string]$Build = 'build\bin\Release',
    [string]$Rom = '',   # N64 ROM path (required for the headless runs)
    [int]$Frames = 1800,
    [string]$TestInput = ''   # e.g. random:42 -> peers get scripted pseudo-random input
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$bin = Join-Path $root $Build
$fail = 0

function Run-Test([string]$name, [string]$exe, [string[]]$args_) {
    $path = Join-Path $bin $exe
    if (-not (Test-Path $path)) { Write-Output "MISSING $exe (build first)"; $script:fail++; return }
    $out = & $path @args_ 2>&1
    $code = $LASTEXITCODE
    $out | Select-Object -Last 12 | ForEach-Object { "  $_" } | Write-Output
    if ($code -ne 0) { Write-Output "FAIL $name (exit $code)"; $script:fail++ } else { Write-Output "PASS $name" }
}

Write-Output "== lockstep: 1 to 4 peers, loss/reorder/dup, 50k samples"
Run-Test 'lockstep' 'n64run_lockstep_test.exe' @()

Write-Output "== save sync: simulated lossy network + limits"
Run-Test 'save-sync-sim' 'n64run_save_sync_test.exe' @()
if ($Server) {
    Write-Output "== save sync: two real sessions through $Server"
    Run-Test 'save-sync-live' 'n64run_save_sync_test.exe' @($Server)
    Write-Output "== hubnet: direct UDP and forced relay"
    Run-Test 'hubnet-direct' 'hubnet_test.exe' @($Server, '4', '300')
    $old = $env:HUBNET_FORCE_RELAY
    $env:HUBNET_FORCE_RELAY = '1'
    Run-Test 'hubnet-relay' 'hubnet_test.exe' @($Server, '4', '300')
    $env:HUBNET_FORCE_RELAY = $old
}

# ---- headless emulation (no window, no audio, no speed limit)
. (Join-Path $PSScriptRoot 'n64run_headless_lib.ps1')
$n64 = Join-Path $bin 'n64run.exe'
if (-not (Test-Path $n64) -or -not $Rom -or -not (Test-Path -LiteralPath $Rom)) {
    Write-Output "SKIP headless runs (n64run.exe or ROM missing)"
} else {
    $tmp = Join-Path $env:TEMP ("n64qa-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory $tmp | Out-Null

    Write-Output "== headless offline: 1200 VIs, screenshot + RAM dump, no visible window"
    $a = "--rom `"$Rom`" --game mk64 --save-dir `"$tmp\save0`" --headless --frames 1200 --screenshot `"$tmp\shot.png`" --dump-ram 0x0,64,`"$tmp\ram.bin`""
    $r = Wait-HeadlessRun (Start-HeadlessRun $n64 $a "$tmp\off") "$tmp\off" 180
    $ok = ($r.Exit -eq 0) -and ($r.MaxVisible -eq 0) -and (Test-Path "$tmp\shot.png") -and ((Get-Item "$tmp\ram.bin").Length -eq 64) -and ($r.Err -match 'headless: 1200 VIs')
    Write-Output ("  exit={0} visibleWindows={1} {2}s" -f $r.Exit, $r.MaxVisible, $r.Seconds)
    if ($ok) { Write-Output "PASS headless-offline" } else { Write-Output "FAIL headless-offline"; $r.Err | Select-Object -Last 8 | ForEach-Object { "  $_" }; $fail++ }

    if ($Server) {
        Write-Output "== headless online: two peers through $Server, $Frames VIs"
        $room = ('{0}-1' -f ([guid]::NewGuid().ToString('N').Substring(0, 6).ToUpper() -replace '[01OIL]', 'X'))
        $items = @()
        foreach ($slot in 0, 1) {
            $a = "--rom `"$Rom`" --game mk64 --save-dir `"$tmp\savep$slot`" --net $Server --room $room --players 2 --ports 0,1,-1,-1 --slot $slot --headless --frames $Frames" + (Get-TestInputArg $TestInput)
            $items += @{ P = (Start-HeadlessRun $n64 $a "$tmp\peer$slot"); Base = "$tmp\peer$slot" }
        }
        $res = Wait-HeadlessRuns $items 300
        $hashes = @()
        foreach ($slot in 0, 1) {
            $h = @{}
            $res[$slot].Err | Where-Object { $_ -match 'sync check sample (\d+) RDRAM ([0-9a-f]+)' } | ForEach-Object { $h[[int]$Matches[1]] = $Matches[2] }
            $hashes += , $h
            Write-Output ("  peer{0}: exit={1} visibleWindows={2} syncChecks={3} {4}s" -f $slot, $res[$slot].Exit, $res[$slot].MaxVisible, $h.Count, $res[$slot].Seconds)
        }
        $common = @($hashes[0].Keys | Where-Object { $hashes[1].ContainsKey($_) })
        $mismatch = @($common | Where-Object { $hashes[0][$_] -ne $hashes[1][$_] })
        $desync = @($res | Where-Object { $_.Err -match 'DESYNC' }).Count
        $ok = ($res[0].Exit -eq 0) -and ($res[1].Exit -eq 0) -and ($res[0].MaxVisible + $res[1].MaxVisible -eq 0) -and ($common.Count -ge 2) -and ($mismatch.Count -eq 0) -and ($desync -eq 0)
        Write-Output ("  common sync checks={0} mismatches={1} desync lines={2}" -f $common.Count, $mismatch.Count, $desync)
        if ($ok) { Write-Output "PASS headless-online-2peer" }
        else { Write-Output "FAIL headless-online-2peer"; $res | ForEach-Object { $_.Err | Select-Object -Last 6 | ForEach-Object { "  $_" } }; $fail++ }
    }
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# Argument validation the hub relies on. Each must be rejected (exit 2) without touching the ROM or SDL.
Write-Output "== n64run argument checks (must be rejected, never run)"
$exe = Join-Path $bin 'n64run.exe'
$cases = @(
    # Sparse slots are legal (--players counts PCs, not the highest slot), so these check the real limits instead.
    @{ name = 'ports name 2 PCs but --players 1';          args = '--rom x.z64 --net wss://h --room ABC234-1 --slot 2 --players 1 --ports 0,2,-1,-1'; expect = 'bad --players/--slot' },
    @{ name = 'ports value out of range (slot 4)';         args = '--rom x.z64 --net wss://h --room ABC234-1 --slot 0 --players 2 --ports 0,4,-1,-1'; expect = 'bad --players/--slot' },
    @{ name = 'share-host-save without --net';             args = '--rom x.z64 --share-host-save'; expect = 'needs --net' }
)
foreach ($c in $cases) {
    $o = & $exe ($c.args -split ' ') 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($code -eq 2 -and $o -match [regex]::Escape($c.expect)) { Write-Output "PASS $($c.name)" }
    else { Write-Output "FAIL $($c.name) (exit $code): $o"; $fail++ }
}

if ($fail) { Write-Output "FAILED ($fail)"; exit 1 }
Write-Output "ALL PASSED"
exit 0

