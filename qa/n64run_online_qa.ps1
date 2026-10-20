# Headless QA for N64 online play (n64run lockstep + host-save sync + hub launch arguments).
# Windows PowerShell 5.1. Opens no windows: console tests only, and n64run is only ever run with
# arguments it rejects during argument parsing (it exits before SDL or any window exists).
#
# Usage:
#   powershell -File qa\n64run_online_qa.ps1                 (offline-safe checks only)
#   powershell -File qa\n64run_online_qa.ps1 -Server wss://smash-netplay.jumpcircuit.workers.dev
#   $env:HUBNET_FORCE_RELAY=1 ... (forces the WebSocket relay path for the live tests)
param(
    [string]$Server = '',
    [string]$Build = 'build\bin\Release'
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

# Argument validation the hub relies on. Each must be rejected (exit 2) without touching the ROM or SDL.
Write-Output "== n64run argument checks (must be rejected, never run)"
$exe = Join-Path $bin 'n64run.exe'
$cases = @(
    @{ name = 'gap lobby slots {0,2}: slot 2 of 2 players'; args = '--rom x.z64 --net wss://h --room ABC234-1 --slot 2 --players 2 --ports 0,2,-1,-1'; expect = 'bad --players/--slot' },
    @{ name = 'ports reference slot >= players';          args = '--rom x.z64 --net wss://h --room ABC234-1 --slot 0 --players 3 --ports 0,2,3,-1'; expect = 'references a slot' },
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
