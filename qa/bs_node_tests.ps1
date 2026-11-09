# Runs the BattleShip server/cloudflare node test suite through its own runner, run_all_tests.mjs.
# Every ws/http test gets a FRESH local `wrangler dev` with the per-test --var flags from run_all_tests.mjs's SUITE table.
# Never production: the runner starts local wranglers only, and it kills only the wranglers it started, by PID
# (run_all_tests.mjs and test_v2_lib.mjs). Runs node inline (no new windows).
# Usage:
#   powershell -NoProfile -File qa\bs_node_tests.ps1                              all test files
#   powershell -NoProfile -File qa\bs_node_tests.ps1 -Only test_hostgrace.mjs     one file (or any name substring)
#   powershell -NoProfile -File qa\bs_node_tests.ps1 -Only a.mjs,b.mjs -Cf <dir> -PortBase 10300
# -Cf defaults to $env:BS_WORKER_DIR, else BattleShip\server\cloudflare. -PortBase moves every port (default 10300, so a run never
# collides with a plain `node run_all_tests.mjs` on its historic ports 10100+).
# Exit 0 only if every run exits 0 and at least one test file ran. Exit 2 if node or wrangler is missing.
param(
    [string]$Cf = 'C:\Users\fatty\Desktop\SmashClassicPort\BattleShip\server\cloudflare',
    [Alias('BasePort')][int]$PortBase = 10300,
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Continue'
if (-not $PSBoundParameters.ContainsKey('Cf') -and $env:BS_WORKER_DIR) { $Cf = $env:BS_WORKER_DIR }

if (-not (Get-Command node -ErrorAction SilentlyContinue)) { Write-Host 'bs_node_tests: node was not found on PATH.'; exit 2 }
if (-not (Test-Path "$Cf\node_modules\wrangler\bin\wrangler.js")) {
    Write-Host "bs_node_tests: wrangler is not installed in $Cf (run npm install there first)."; exit 2
}
if (-not (Test-Path "$Cf\run_all_tests.mjs")) { Write-Host "bs_node_tests: $Cf\run_all_tests.mjs not found."; exit 2 }

$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })   # `powershell -File ... -Only a,b` binds one string
$filters = if ($Only.Count) { $Only } else { @('') }
$env:SUITE_PORT_BASE = "$PortBase"
$total = 0; $passed = 0; $worst = 0
Push-Location $Cf
try {
    foreach ($f in $filters) {
        if ($f) { Write-Host "=== bs_node_tests filter: $f ===" }
        $captured = @()
        if ($f) { & node run_all_tests.mjs $f | Tee-Object -Variable captured | Out-Host }
        else { & node run_all_tests.mjs | Tee-Object -Variable captured | Out-Host }
        $rc = $LASTEXITCODE
        if ($rc -ne 0) { $worst = $rc }   # a crash comes back as a negative code: never let a later pass hide it
        $summary = @($captured | Where-Object { $_ -match '(\d+)/(\d+) test files passed' })
        if ($summary.Count) {
            $m = [regex]::Match($summary[-1], '(\d+)/(\d+) test files passed')
            $passed += [int]$m.Groups[1].Value; $total += [int]$m.Groups[2].Value
        } else {
            Write-Host "bs_node_tests: runner printed no summary for filter '$f' (exit $rc)."
            if ($worst -eq 0) { $worst = 1 }
        }
    }
} finally { Pop-Location }

Write-Host "bs_node_tests: $passed/$total test files passed"
if ($worst -ne 0 -or $total -eq 0) { exit 1 }
exit 0
