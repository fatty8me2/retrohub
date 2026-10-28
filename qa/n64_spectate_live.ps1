# Headless N64 live-spectate QA (Windows PowerShell 5.1): two n64run players (real-time paced) + one late spectator through a LOCAL worker.
# Hidden and muted. The spectator must join from a live snapshot (not sample 0), verify the loaded RDRAM against the host, get live, and
# then agree with the host on every common RDRAM sync-check hash.
#   powershell -File qa\n64_spectate_live.ps1 -Rom "C:\...\Mario Kart 64 (U) [!].z64" [-LateSec 25] [-Frames 7200] [-Relay] [-NoLive]
# -NoLive: N64RUN_NO_LIVE_SPECTATE=1 on the spectator (old-client path: replay from 0) -> expects a replay join instead.
# -Server: reuse a running local worker; otherwise one is started from -WorkerDir on -Port with a fresh persist dir and only its PID is stopped.
param(
    [Parameter(Mandatory = $true)][string]$Rom,
    [string]$Server = '',
    [string]$WorkerDir = 'C:\Users\fatty\Desktop\SmashClassicPort\BattleShip\server\cloudflare',
    [int]$Port = 8761,
    [string]$EmuDir = 'C:\Users\fatty\Desktop\SmashClassicPort\RetroHub\deps\emu',
    [int]$Frames = 7200,
    [int]$LateSec = 25,
    [int]$TimeoutSec = 400,
    [switch]$Relay,
    [switch]$NoLive
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. "$PSScriptRoot\n64run_headless_lib.ps1"
$wr = $null
$tmp = Join-Path $env:TEMP ("n64spec-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $tmp | Out-Null
if ($Server -eq '') {
    $Server = "ws://127.0.0.1:$Port"
    $wl = Join-Path $tmp 'wrangler.log'
    $node = (Get-Command node).Source
    $wr = Start-Process -FilePath $node -ArgumentList @('node_modules\wrangler\bin\wrangler.js', 'dev', '--ip', '127.0.0.1', '--port', $Port, '--persist-to', "$tmp\persist") `
        -WorkingDirectory $WorkerDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $wl -RedirectStandardError "$wl.err"
    $up = $false
    for ($i = 0; $i -lt 90 -and -not $up; $i++) {
        Start-Sleep -Seconds 1
        try { $null = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$Port/" -TimeoutSec 2; $up = $true } catch { if ($_.Exception.Response) { $up = $true } }
    }
    if (-not $up) { Stop-Process -Id $wr.Id -Force; throw "local worker did not start (see $wl)" }
}
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' }
$n64 = Join-Path $root 'build\bin\Release\n64run.exe'
$room = ('{0}-1' -f ([guid]::NewGuid().ToString('N').Substring(0, 6).ToUpper() -replace '[01OIL]', 'X'))
$items = @()
foreach ($slot in 0, 1) {
    $a = "--rom `"$Rom`" --emu-dir `"$EmuDir`" --game mk64 --save-dir `"$tmp\save$slot`" --net $Server --room $room --players 2 --ports 0,1,-1,-1 --slot $slot --headless --realtime --frames $Frames --test-input random:42"
    $items += @{ P = (Start-HeadlessRun $n64 $a "$tmp\peer$slot"); Base = "$tmp\peer$slot" }
}
Start-Sleep -Seconds $LateSec
if ($NoLive) { $env:N64RUN_NO_LIVE_SPECTATE = '1' }
$a = "--rom `"$Rom`" --emu-dir `"$EmuDir`" --game mk64 --save-dir `"$tmp\saves`" --net $Server --room $room --players 2 --ports 0,1,-1,-1 --spectate --headless"
$items += @{ P = (Start-HeadlessRun $n64 $a "$tmp\spec"); Base = "$tmp\spec" }
Remove-Item Env:\N64RUN_NO_LIVE_SPECTATE -ErrorAction SilentlyContinue
$res = Wait-HeadlessRuns $items $TimeoutSec
if ($wr) { try { Stop-Process -Id $wr.Id -Force } catch {} }

function Hashes($lines) { $h = @{}; $lines | Where-Object { $_ -match 'sync check sample (\d+) RDRAM ([0-9a-f]+)' } | ForEach-Object { $h[[int]$Matches[1]] = $Matches[2] }; return $h }
$hh = Hashes $res[0].Err
$sh = Hashes $res[2].Err
$common = @($sh.Keys | Where-Object { $hh.ContainsKey($_) } | Sort-Object)
$bad = @($common | Where-Object { $hh[$_] -ne $sh[$_] })
$spec = $res[2].Err
$fail = 0
foreach ($i in 0, 1, 2) { Write-Output ("  proc{0}: exit={1} visible={2} {3}s" -f $i, $res[$i].Exit, $res[$i].MaxVisible, $res[$i].Seconds) }
$spec | Where-Object { $_ -match 'spectate' } | Select-Object -First 14 | ForEach-Object { "  spec: $_" }
$res[0].Err | Where-Object { $_ -match 'spectate|frame-gap' } | Select-Object -First 10 | ForEach-Object { "  host: $_" }
$span = if ($common.Count) { "samples $($common[0])..$($common[-1])" } else { 'none' }
Write-Output ("  common sync checks={0} ({1}) mismatches={2}" -f $common.Count, $span, $bad.Count)
if ($NoLive) {
    if (-not ($spec -match 'replay from sample 0')) { Write-Output 'FAIL expected a replay-from-0 join'; $fail++ }
} else {
    if (-not ($spec -match 'live join verified')) { Write-Output 'FAIL no verified live join'; $fail++ }
    if (-not ($spec -match 'LIVE at sample .*live snapshot')) { Write-Output 'FAIL never got live from the snapshot'; $fail++ }
}
if ($common.Count -lt 6 -or $bad.Count -ne 0) { Write-Output 'FAIL hash agreement'; $fail++ }
if (($res | Where-Object { $_.MaxVisible -gt 0 }).Count) { Write-Output 'FAIL a visible window'; $fail++ }
if ($res[0].Exit -ne 0 -or $res[1].Exit -ne 0) { Write-Output 'FAIL a player did not exit cleanly'; $fail++ }
Write-Output "  logs: $tmp"
if ($fail) { Write-Output "FAILED ($fail)"; exit 1 }
Write-Output 'PASS'
exit 0
