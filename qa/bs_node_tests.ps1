# Runs every BattleShip server/cloudflare node test, each against a FRESH local `wrangler dev` (isolated registry, short persist dir
# C:\rhq\<port>, ports 9900+). Tests that start their own wrangler (stats, r2budget, saves) run bare. Kills only the PIDs it started.
# Usage: powershell -File qa\bs_node_tests.ps1 [-Cf <BattleShip\server\cloudflare>] [-BasePort 9900] [-Only test_room4.mjs,...]
param([string]$Cf = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'qa-124-bs\server\cloudflare'), [int]$BasePort = 9900, [string[]]$Only = @())
$ErrorActionPreference = 'Continue'
$self = @('test_stats.mjs', 'test_r2budget.mjs', 'test_saves.mjs')
$tests = Get-ChildItem $Cf -Filter 'test_*.mjs' | Sort-Object Name
if ($Only.Count) { $tests = $tests | Where-Object { $Only -contains $_.Name } }
$port = $BasePort
$results = @()
$env:WRANGLER_REGISTRY_PATH = Join-Path $env:TEMP ('wr_reg_' + [guid]::NewGuid().ToString('N').Substring(0, 6))
New-Item -ItemType Directory -Force $env:WRANGLER_REGISTRY_PATH | Out-Null
function Kill-Tree([int]$id) { if ($id) { & taskkill /PID $id /T /F 2>&1 | Out-Null } }
foreach ($t in $tests) {
    $port++
    $log = Join-Path $env:TEMP "bsn_$($t.BaseName).log"
    $wp = $null
    $isSelf = $self -contains $t.Name
    $head = (Get-Content $t.FullName -TotalCount 10) -join "`n"   # the header documents the --var flags and the URL scheme each test needs
    $vars = @(); foreach ($m in [regex]::Matches($head, '--var\s+(\S+)')) { $vars += '--var'; $vars += $m.Groups[1].Value }
    $scheme = if ($head -match 'node \S+\.mjs \[http://') { 'http' } else { 'ws' }
    if (-not $isSelf) {
        $persist = "C:\rhq\$port"
        Remove-Item -Recurse -Force $persist -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $persist | Out-Null
        $wp = Start-Process node -PassThru -WindowStyle Hidden -WorkingDirectory $Cf -RedirectStandardOutput "$log.wr" -RedirectStandardError "$log.wre" `
            -ArgumentList (@('node_modules/wrangler/bin/wrangler.js', 'dev', '-c', 'wrangler.jsonc', '--ip', '127.0.0.1', '--port', "$port", '--persist-to', $persist) + $vars)
        $up = $false
        for ($i = 0; $i -lt 90 -and -not $up; $i++) {
            Start-Sleep -Milliseconds 1000
            try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1', $port); $c.Close(); $up = $true } catch { }
        }
        if (-not $up) { Kill-Tree $wp.Id; $results += [pscustomobject]@{ Test = $t.Name; Result = 'FAIL'; Note = 'wrangler did not start' }; continue }
        Start-Sleep -Milliseconds 1500
    }
    $args = @($t.Name); if (-not $isSelf) { $args += "${scheme}://127.0.0.1:$port" }
    $envPorts = @{ BUDGET_TEST_PORT = "$($port + 500)"; SAVES_TEST_PORT = "$($port + 500)"; STATS_TEST_PORT = "$($port + 500)" }
    foreach ($k in $envPorts.Keys) { Set-Item "env:$k" $envPorts[$k] }
    $np = Start-Process node -PassThru -WindowStyle Hidden -WorkingDirectory $Cf -RedirectStandardOutput $log -RedirectStandardError "$log.err" -ArgumentList $args
    $null = $np.Handle
    if (-not $np.WaitForExit(240000)) { Kill-Tree $np.Id; $res = 'FAIL'; $note = 'timeout' }
    else { $res = if ($np.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }; $note = "exit $($np.ExitCode)" }
    $out = (Get-Content $log -ErrorAction SilentlyContinue) -join "`n"
    $fails = ([regex]::Matches($out, '(?m)^FAIL')).Count
    if ($fails) { $res = 'FAIL'; $note += ", $fails FAIL lines" }
    $oks = ([regex]::Matches($out, '(?m)^ok')).Count
    $results += [pscustomobject]@{ Test = $t.Name; Result = $res; Note = "$note, $oks ok" }
    if ($wp) { Kill-Tree $wp.Id }
    Remove-Item "$Cf\wrangler.test.jsonc", "$Cf\wrangler.budget.jsonc" -ErrorAction SilentlyContinue
}
$results | Format-Table -AutoSize
$bad = @($results | Where-Object { $_.Result -ne 'PASS' }).Count
Write-Host "bs_node_tests: $($results.Count - $bad)/$($results.Count) passed"
if ($bad) { exit 1 }
