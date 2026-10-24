# Two hidden headless n64run peers, real-time pacing, real audio plugin on SDL's silent dummy driver, simulated
# RTT/jitter and periodic injected stalls. Logs 'pace:' (wall-clock lag), 'latency:' (delay/RTT) lines.
#   powershell -NoProfile -File qa\n64_pace_qa.ps1 -N64Exe <exe> -Rom <rom> -Tag after [-NoFix 1] [-Frames 12000]
param(
    [Parameter(Mandatory = $true)][string]$N64Exe,
    [Parameter(Mandatory = $true)][string]$Rom,
    [string]$Tag = 'run',
    [int]$Frames = 12000,
    [int]$NoFix = 0,
    [int]$LatencyMs = 40, [int]$JitterMs = 15,
    [string]$Stall = '1800,900',
    [string]$Server = 'ws://127.0.0.1:8793',
    [string]$EmuDir = ''
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$rh = Split-Path -Parent $PSScriptRoot
if (-not $EmuDir) { $EmuDir = Join-Path $rh 'deps\emu' }
. (Join-Path $rh 'qa\n64run_headless_lib.ps1')
$tmp = Join-Path $env:TEMP ('n64pace-' + $Tag + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
New-Item -ItemType Directory $tmp | Out-Null
$env:RETROHUB_QA = '1'; $env:HUBNET_LOOPBACK = '1'; $env:N64RUN_QA_AUDIO = '1'; $env:N64RUN_QA_STALL = $Stall
$env:HUBNET_SIM_ENABLE = '1'; $env:HUBNET_SIM_LATENCY_MS = "$LatencyMs"; $env:HUBNET_SIM_JITTER_MS = "$JitterMs"
if ($NoFix) { $env:N64RUN_QA_NOFIX = '1' } else { Remove-Item Env:N64RUN_QA_NOFIX -ErrorAction SilentlyContinue }
$room = ('{0}-1' -f ([guid]::NewGuid().ToString('N').Substring(0, 6).ToUpper() -replace '[01OIL]', 'X'))
$items = @()
for ($s = 0; $s -lt 2; $s++) {
    $base = Join-Path $tmp "p$s"
    $a = "--rom `"$Rom`" --game mariogolf --emu-dir `"$EmuDir`" --save-dir `"$base-save`" --net $Server --room $room --players 2 --ports 0,1,-1,-1 --slot $s --headless --realtime --frames $Frames --test-input random:1"
    $items += @{ P = (Start-HeadlessRun $N64Exe $a $base); Base = $base }
}
$res = Wait-HeadlessRuns $items 600
for ($s = 0; $s -lt 2; $s++) { Copy-Item "$($items[$s].Base).err.txt" (Join-Path $PSScriptRoot "pace-$Tag-p$s.txt") -ErrorAction SilentlyContinue }
"exit: $($res[0].Exit) $($res[1].Exit) secs $($res[0].Seconds) visible windows $($res[0].MaxVisible)/$($res[1].MaxVisible)"
