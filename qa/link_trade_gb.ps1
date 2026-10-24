# Headless two-peer LINK-CABLE TRADE QA with a real Pokemon ROM (Gen 1: Red). Windows PowerShell 5.1.
# Two retrorun peers (sameboy_rb rollback core, --link dual Game Boy) talk through a LOCAL room server. Scripted input
# (RETRORUN_INPUT_SCRIPT, see qa\link_trade_mk.py) walks both consoles from the saved Cable Club counter to the Trade Center and trades.
# PASS = both peers exit 0, identical final hashes, no DESYNC, and the serialized core state (RETRORUN_STATE_DUMP) shows the two parties swapped.
# Needs: a retrorun built with the QA input script + state dump (see report), a local worker, the ROM zip (read only, extracted to qa\out).
#   cd BattleShip\server\cloudflare; npx wrangler dev --ip 127.0.0.1 --port 8761 --persist-to <fresh dir>
# Usage: powershell -File qa\link_trade_gb.ps1 [-Server ws://127.0.0.1:8761] [-Game red] [-Lockstep] [-Relay] [-Bad] [-Core <dll>]
param(
    [string]$Server = 'ws://127.0.0.1:8761',
    [string]$Game = 'red',
    [string]$RomDir = 'C:\Users\fatty\Desktop\Everything\Games\GBC',
    [string]$Exe = '',
    [string]$Core = '',
    [switch]$Lockstep,   # stock SameBoy lockstep core instead of the rollback core
    [switch]$Relay, [switch]$Bad,
    [string]$Tag = ''
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Tag) { $Tag = "link_trade_$Game" }
$games = @{ red = @{ Zip = 'Pokemon - Red Version*.zip'; Rom = 'red.gb'; Sys = 'gb'; A = 0xB0; B = 0xB1 } }
$g = $games[$Game]; if (-not $g) { Write-Output "unknown game $Game"; exit 2 }
if (-not $Exe) { $Exe = Join-Path $root 'build\bin\Release\retrorun.exe' }
if (-not $Core) { $Core = if ($Lockstep) { Join-Path $root 'deps\cores\sameboy_libretro.dll' } else { Join-Path $root 'build\bin\Release\cores\sameboy_rb_libretro.dll' } }
foreach ($p in $Exe, $Core) { if (-not (Test-Path $p)) { Write-Output "missing $p"; exit 2 } }
if (-not ([IO.File]::ReadAllText($Exe, [Text.Encoding]::GetEncoding(28591)).Contains('RETRORUN_STATE_DUMP'))) { Write-Output 'FAIL: this retrorun has no RETRORUN_INPUT_SCRIPT/RETRORUN_STATE_DUMP support (apply the QA patch)'; exit 2 }

$out = Join-Path $root "qa\out\$Tag"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $out | Out-Null
# ROM: extract the zip (never modify the original) into qa\out\link_trade_roms
$romCache = Join-Path $root 'qa\out\link_trade_roms'; New-Item -ItemType Directory -Force $romCache | Out-Null
$rom = Join-Path $romCache $g.Rom
if (-not (Test-Path $rom)) {
    $zip = Get-ChildItem -Path $RomDir -Filter $g.Zip | Select-Object -First 1
    if (-not $zip) { Write-Output "FAIL: ROM zip $($g.Zip) not found in $RomDir"; exit 2 }
    $tmp = Join-Path $romCache ('x_' + $Game); Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $zip.FullName -DestinationPath $tmp -Force
    Move-Item (Get-ChildItem $tmp -File | Select-Object -First 1).FullName $rom; Remove-Item -Recurse -Force $tmp
}
$env:RETRORUN_MUTE = '1'
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }
if ($Bad) { $env:HUBNET_SIM_ENABLE = '1'; $env:HUBNET_SIM_LATENCY_MS = '60'; $env:HUBNET_SIM_JITTER_MS = '30'; $env:HUBNET_SIM_LOSS_PCT = '5' }
else { Remove-Item Env:HUBNET_SIM_ENABLE, Env:HUBNET_SIM_LATENCY_MS, Env:HUBNET_SIM_JITTER_MS, Env:HUBNET_SIM_LOSS_PCT -ErrorAction SilentlyContinue }
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; $rnd = New-Object System.Random
$room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
$stem = [IO.Path]::GetFileNameWithoutExtension($rom)
$frames = 0
foreach ($i in 0, 1) { $f = [int]([regex]::Match((Get-Content (Join-Path $PSScriptRoot "link_trade_${Game}_p$i.txt") -Raw), '# frames (\d+)').Groups[1].Value); if ($f -gt $frames) { $frames = $f } }
$procs = @()
foreach ($slot in 0, 1) {
    $sv = Join-Path $out "sv$slot"; New-Item -ItemType Directory -Force -Path (Join-Path $out "s$slot") | Out-Null; New-Item -ItemType Directory -Force -Path $sv | Out-Null
    Copy-Item (Join-Path $PSScriptRoot ("link_trade_fixtures\$Game" + '_' + @('a', 'b')[$slot] + '.srm')) (Join-Path $sv "$stem.srm")
    $src = Get-Content (Join-Path $PSScriptRoot "link_trade_${Game}_p$slot.txt") -Raw
    $scr = Join-Path $out "script$slot.txt"; $src.Replace('{OUT}', (Join-Path $out "s$slot")) | Set-Content -Encoding ASCII $scr
    $log = Join-Path $out "slot$slot.log"
    $a = @('--core', $Core, '--rom', $rom, '--system', $g.Sys, '--link', '--save-dir', $sv, '--state-dir', (Join-Path $out "st$slot"), '--headless', '--frames', $frames,
        '--net', $Server, '--room', $room, '--slot', $slot, '--players', 2, '--ports', '0,1,-1,-1', '--delay', 'auto', '--log', $log, '--screenshot', (Join-Path $out "final$slot.png"))
    if (-not $Lockstep) { $a += '--rollback' }
    $env:RETRORUN_INPUT_SCRIPT = $scr; $env:RETRORUN_STATE_DUMP = Join-Path $out "state$slot.bin"
    $pr = Start-Process -FilePath $Exe -ArgumentList $a -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $out "err$slot.txt") -RedirectStandardOutput (Join-Path $out "out$slot.txt"); $null = $pr.Handle
    $procs += [pscustomobject]@{ Slot = $slot; Log = $log; Err = (Join-Path $out "err$slot.txt"); Proc = $pr }
}
$t0 = Get-Date
while (@($procs | Where-Object { -not $_.Proc.HasExited }).Count -gt 0) {
    if (((Get-Date) - $t0).TotalSeconds -gt 900) { $procs | ForEach-Object { if (-not $_.Proc.HasExited) { $_.Proc.Kill() } }; Write-Output 'HARD TIMEOUT'; break }
    Start-Sleep -Milliseconds 500
}
$fail = 0; $fin = @{}
foreach ($p in $procs) {
    $p.Proc.WaitForExit()
    $txt = Get-Content $p.Log -Raw; $err = (Get-Content $p.Err -Raw)
    $m = [regex]::Match($txt, 'final: frame (\d+) state-hash (\w+) output-hash (\w+) input-hash (\w+)')
    $fin[$p.Slot] = $m.Groups
    Write-Output ("slot $($p.Slot) exit=$($p.Proc.ExitCode) $($m.Value) desync=$($txt -match 'DESYNC') saves-exchanged=$($txt -match 'saves exchanged')")
    if ($err) { Write-Output ("  stderr: " + $err.Trim()) }
    if ($p.Proc.ExitCode -ne 0 -or -not $m.Success -or $txt -match 'DESYNC') { $fail++ }
}
if ($fin[0] -and $fin[1] -and $fin[0][2].Success -and $fin[1][2].Success) { foreach ($k in 2, 3, 4) { if ($fin[0][$k].Value -ne $fin[1][$k].Value) { Write-Output "MISMATCH hash group $k"; $fail++ } } }
# Party check (Gen 1): find wPlayerName "RED" + wPartyCount(1) + species list in the serialized state; console 1 then console 2.
function Get-Gen1Parties([byte[]]$b) {
    $r = @(); $i = 0
    while (($i = [Array]::IndexOf($b, [byte]0x91, $i)) -ge 0 -and $i -lt $b.Length - 16) {
        if ($b[$i + 1] -eq 0x84 -and $b[$i + 2] -eq 0x83 -and $b[$i + 3] -eq 0x50 -and $b[$i + 11] -eq 1 -and $b[$i + 13] -eq 0xFF) { $r += $b[$i + 12] }
        $i++
    }
    return , $r
}
foreach ($slot in 0, 1) {
    $sf = Join-Path $out "state$slot.bin"
    if (-not (Test-Path $sf)) { Write-Output "slot $slot : no state dump"; $fail++; continue }
    $pt = Get-Gen1Parties ([IO.File]::ReadAllBytes($sf))
    Write-Output ("slot $slot party species (console1,console2): " + (($pt | ForEach-Object { '0x{0:X2}' -f $_ }) -join ','))
    if ($pt.Count -ne 2 -or $pt[0] -ne $g.B -or $pt[1] -ne $g.A) { Write-Output "  expected swapped parties 0x$('{0:X2}' -f $g.B),0x$('{0:X2}' -f $g.A)"; $fail++ }
}
Write-Output "screenshots: $out\s0\*.png (each checkpoint, own+partner console side by side), final0/1.png"
if ($fail -eq 0) { Write-Output "LINK TRADE QA PASS ($Game)" } else { Write-Output "LINK TRADE QA FAIL ($Game, $fail)"; exit 1 }
