# Rollback netplay matrix on mgba_link: stall seconds, rollback depth, desyncs under simulated networks. Headless, muted, kills only its own PIDs.
# Server: start `npx wrangler dev --port 8793 --ip 127.0.0.1` in BattleShip\server\cloudflare first.
# Usage: powershell -File qa\retrorun_rb_matrix.ps1 -BinDir build\bin\Release -Rom <rom.gba> -Case bad60 [-Frames 1800] [-Extra1 100]
param(
    [string]$Server = 'ws://127.0.0.1:8793',
    [string]$BinDir = 'build\bin\Release',
    [string]$Rom,
    [int]$Frames = 1800,
    [ValidateSet('direct', 'relay', 'bad60', 'bad120', 'asym')][string]$Case = 'direct',
    [string]$Extra = '',          # extra args for retrorun (e.g. "--rb-pred 8")
    [switch]$Lockstep,            # plain lockstep (no --rollback)
    [string]$Tag = 'rbm'
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = if ([IO.Path]::IsPathRooted($BinDir)) { $BinDir } else { Join-Path $root $BinDir }
$exe = Join-Path $bin 'retrorun.exe'
$core = Join-Path $bin 'mgba_link_libretro.dll'
$out = Join-Path $root ('qa\out\' + $Tag + '_' + $Case)
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:RETROHUB_QA = "1"; $env:RETRORUN_MUTE = '1'; $env:HUBNET_LOOPBACK = '1'   # bind 127.0.0.1 only (no firewall prompts)
Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue
if ($Case -eq 'relay') { $env:HUBNET_FORCE_RELAY = '1' }
# per-slot one-way simulator settings: latency, jitter, loss
$sim = @{ 0 = @(0, 0, 0); 1 = @(0, 0, 0) }
if ($Case -eq 'bad60') { $sim[0] = @(60, 30, 5); $sim[1] = @(60, 30, 5) }
if ($Case -eq 'bad120') { $sim[0] = @(120, 40, 10); $sim[1] = @(120, 40, 10) }
if ($Case -eq 'asym') { $sim[0] = @(0, 0, 0); $sim[1] = @(100, 0, 0) }
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random
$room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
$procs = @()
foreach ($slot in 0, 1) {
    $s = $sim[$slot]
    if ($s[0] -gt 0 -or $s[2] -gt 0) {
        $env:HUBNET_SIM_ENABLE = '1'; $env:HUBNET_SIM_LATENCY_MS = "$($s[0])"; $env:HUBNET_SIM_JITTER_MS = "$($s[1])"; $env:HUBNET_SIM_LOSS_PCT = "$($s[2])"; $env:HUBNET_SIM_SEED = '4242'
    } else { Remove-Item Env:HUBNET_SIM_ENABLE, Env:HUBNET_SIM_LATENCY_MS, Env:HUBNET_SIM_JITTER_MS, Env:HUBNET_SIM_LOSS_PCT -ErrorAction SilentlyContinue }
    $sv = Join-Path $out ('sv' + $slot); New-Item -ItemType Directory -Force -Path $sv | Out-Null
    $log = Join-Path $out ('slot' + $slot + '.log')
    $a = @('--core', $core, '--rom', $Rom, '--system', 'gba', '--link', $(if ($Lockstep) { '--no-rollback' } else { '--rollback' }), '--save-dir', $sv, '--state-dir', (Join-Path $out ('st' + $slot)),
        '--headless', '--frames', $Frames, '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $slot, '--players', 2,
        '--ports', '0,1,-1,-1', '--delay', 'auto', '--log', $log) + @($Extra -split ' ' | Where-Object { $_ })
    $procs += [pscustomobject]@{ Slot = $slot; Log = $log; Proc = (Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden) }
}
$t0 = Get-Date
while (@($procs | Where-Object { -not $_.Proc.HasExited }).Count -gt 0) {
    if (((Get-Date) - $t0).TotalSeconds -gt 300) { $procs | ForEach-Object { if (-not $_.Proc.HasExited) { $_.Proc.Kill() } }; Write-Output 'HARD TIMEOUT'; break }
    Start-Sleep -Milliseconds 300
}
$fail = 0
foreach ($p in $procs) {
    $txt = Get-Content $p.Log -Raw
    $st = [regex]::Match($txt, 'rollback-stats: [^\r\n]*').Value
    $dn = [regex]::Match($txt, 'done: [^\r\n]*').Value
    $fm = [regex]::Match($txt, 'final: [^\r\n]*').Value
    $ds = $txt -cmatch 'DESYNC at|RESULT: DESYNC'
    Write-Output ("[$Case] slot " + $p.Slot + " exit=" + $p.Proc.ExitCode + " desync=" + $ds)
    Write-Output ("  " + $st); Write-Output ("  " + $dn); Write-Output ("  " + $fm)
    if ($p.Proc.ExitCode -ne 0 -or $ds) { $fail++ }
}
if ($fail) { Write-Output "FAIL ($fail)"; exit 1 } else { Write-Output 'OK' }
