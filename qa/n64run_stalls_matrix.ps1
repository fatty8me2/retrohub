# Stall matrix: random input on every peer, local wrangler dev, optional --realtime pacing.
#   powershell -NoProfile -File matrix.ps1 -N64Exe <exe> -Tag before [-Realtime 1] [-Games sf64,pokestadium2] [-Scen 2p,4p,relay,sim,4prelay]
param(
    [string]$N64Exe,
    [string]$Tag = 'run',
    [int]$Frames = 3000,
    [int]$Realtime = 1,
    [string]$Games = 'sf64,pokestadium2',
    [string]$Scen = '2p,4p,relay,sim',
    [string]$Server = 'ws://127.0.0.1:8791',
    [string]$EmuDir = 'C:\Users\fatty\Desktop\SmashClassicPort\RetroHub\deps\emu',
    [string]$TestInput = 'random:1'
)
$ErrorActionPreference = 'Continue'
$rh      = 'C:\Users\fatty\Desktop\SmashClassicPort\RetroHub'
$romDir  = 'C:\Users\fatty\Desktop\Everything\Games\N64'
$scratch = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $rh 'qa\n64run_headless_lib.ps1')
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$logDir  = Join-Path $scratch ("logs-$Tag-$stamp")
New-Item -ItemType Directory $logDir | Out-Null
$tmpRoot = Join-Path $env:TEMP ('n64stalls-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $tmpRoot | Out-Null
$resultFile = Join-Path $scratch ("results-$Tag-$stamp.txt")
function New-Room { ('{0}-1' -f ([guid]::NewGuid().ToString('N').Substring(0, 6).ToUpper() -replace '[01OIL]', 'X')) }
function Emit([string]$s) { Add-Content -LiteralPath $resultFile -Value $s -Encoding utf8; Write-Output $s }

function Run-Netplay([string]$label, [string]$game, [string]$rom, [int]$peers, [hashtable]$envv) {
    $room = New-Room
    $ports = @{ 2 = '0,1,-1,-1'; 4 = '0,1,2,3' }[$peers]
    $old = @{}
    foreach ($k in $envv.Keys) { $old[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $envv[$k]) }
    $rt = $(if ($Realtime) { '--realtime' } else { '' })
    $items = @()
    for ($s = 0; $s -lt $peers; $s++) {
        $base = Join-Path $tmpRoot ("$label-p$s")
        $a = "--rom `"$rom`" --game $game --emu-dir `"$EmuDir`" --save-dir `"$base-save`" --net $Server --room $room --players $peers --ports $ports --slot $s --headless $rt --frames $Frames --test-input $TestInput"
        $items += @{ P = (Start-HeadlessRun $N64Exe $a $base); Base = $base }
    }
    $res = Wait-HeadlessRuns $items 900
    foreach ($k in $envv.Keys) { [Environment]::SetEnvironmentVariable($k, $old[$k]) }
    $maps = @(); $fcs = @(); $desync = 0; $lost = 0; $exits = @(); $vis = 0; $secs = 0; $stallN = 0; $sampN = 0; $stallMs = 0; $delay = ''
    for ($s = 0; $s -lt $peers; $s++) {
        $r = $res[$s]; $m = @{}; $lines = @($r.Err)
        Copy-Item "$($items[$s].Base).err.txt" (Join-Path $logDir "$label-p$s.err.txt") -ErrorAction SilentlyContinue
        $lines | Where-Object { $_ -match 'sync check sample (\d+) RDRAM ([0-9a-f]+)' } | ForEach-Object { $m[[int]$Matches[1]] = $Matches[2] }
        $maps += , $m
        $fc = -1
        $lines | Where-Object { $_ -match 'headless: (\d+) VIs' } | ForEach-Object { $fc = [int]$Matches[1] }
        $fcs += $fc
        $desync += @($lines | Where-Object { $_ -match 'DESYNC' }).Count
        $lost += @($lines | Where-Object { $_ -match 'Connection lost' }).Count
        $lines | Where-Object { $_ -match 'total (\d+) ms over (\d+) stalled samples, (\d+) samples \(delay (\d+)' } | ForEach-Object { $stallMs += [int]$Matches[1]; $stallN += [int]$Matches[2]; $sampN += [int]$Matches[3]; $delay = $Matches[4] }
        $exits += $(if ($r.TimedOut) { 'TO' } else { "$($r.Exit)" })
        $vis = [math]::Max($vis, $r.MaxVisible); $secs = [math]::Max($secs, $r.Seconds)
    }
    $common = 0; $mism = 0
    for ($s = 1; $s -lt $peers; $s++) { foreach ($k in $maps[0].Keys) { if ($maps[$s].ContainsKey($k)) { $common++; if ($maps[0][$k] -ne $maps[$s][$k]) { $mism++ } } } }
    $pct = $(if ($sampN) { [math]::Round(100.0 * $stallN / $sampN, 1) } else { -1 })
    $msps = $(if ($sampN) { [math]::Round($stallMs / $sampN, 2) } else { -1 })
    Emit ("{0,-18} D={1} stall%={2,5} msPerSample={3,5} stalledMs={4} desync={5} mismatch={6}/{7} lost={8} exits={9} frames={10} secs={11} vis={12}" -f `
        $label, $delay, $pct, $msps, $stallMs, $desync, $mism, $common, $lost, ($exits -join '/'), ($fcs -join '/'), $secs, $vis)
}

$sim   = @{ HUBNET_SIM_ENABLE = '1'; HUBNET_SIM_LATENCY_MS = '80'; HUBNET_SIM_JITTER_MS = '20'; HUBNET_SIM_LOSS_PCT = '3'; HUBNET_SIM_SEED = '7' }
$relay = @{ HUBNET_FORCE_RELAY = '1' }
$none  = @{}
$roms = @{ sf64 = 'Star Fox 64 (U) [!].z64'; pokestadium2 = 'Pokemon Stadium 2 (U) [!].z64'; ssb64rom = 'Super Smash Bros. (USA).n64'; remix = 'Smash_Remix_2.0.0.z64' }
Emit "== stalls matrix tag=$Tag input=$TestInput frames=$Frames realtime=$Realtime server=$Server exe=$N64Exe $(Get-Date -Format s)"
foreach ($g in ($Games -split ',')) {
    $rom = Join-Path $romDir $roms[$g]
    foreach ($sc in ($Scen -split ',')) {
        switch ($sc) {
            '2p'      { Run-Netplay "$g-2p" $g $rom 2 $none }
            '4p'      { Run-Netplay "$g-4p" $g $rom 4 $none }
            'relay'   { Run-Netplay "$g-relay" $g $rom 2 $relay }
            '4prelay' { Run-Netplay "$g-4prelay" $g $rom 4 $relay }
            'sim'     { Run-Netplay "$g-sim" $g $rom 2 $sim }
            '4psim'   { Run-Netplay "$g-4psim" $g $rom 4 $sim }
        }
    }
}
Remove-Item $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
Emit "done"
