# Headless two-console link-cable netplay QA for retrorun --link (SameBoy dual instance). Windows PowerShell 5.1.
# Each peer runs BOTH Game Boys in one deterministic simulation and owns one console; saves are exchanged through the room.
# Start a local server first (hidden): in a BattleShip server\cloudflare worktree: npx wrangler dev --port 8791 --ip 127.0.0.1
# Usage: powershell -File qa\retrorun_link.ps1 -Server ws://127.0.0.1:8791 -Rom out\roms\red.gb -Frames 3000 [-Relay] [-Bad]
param(
    [string]$Server = 'ws://127.0.0.1:8791',
    [string]$Rom = 'deps\testroms\dmg-acid2.gb',
    [int]$Frames = 3000,
    [switch]$Relay,    # HUBNET_FORCE_RELAY=1
    [switch]$Rollback, # GGPO rollback with the patched sameboy_rb core (what the hub launches for GB/GBC link)
    [switch]$Bad,      # simulated bad network: 60 ms +/- 30 ms, 5% loss
    [string]$Tag = 'link'
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$core = if ($Rollback) { Join-Path $root 'build\bin\Release\cores\sameboy_rb_libretro.dll' } else { Join-Path $root 'deps\cores\sameboy_libretro.dll' }
$out = Join-Path $root ('qa\out\' + $Tag)
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:RETRORUN_MUTE = '1'
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }
if ($Bad) { $env:HUBNET_SIM_ENABLE = '1'; $env:HUBNET_SIM_LATENCY_MS = '60'; $env:HUBNET_SIM_JITTER_MS = '30'; $env:HUBNET_SIM_LOSS_PCT = '5' }
else { Remove-Item Env:HUBNET_SIM_ENABLE, Env:HUBNET_SIM_LATENCY_MS, Env:HUBNET_SIM_JITTER_MS, Env:HUBNET_SIM_LOSS_PCT -ErrorAction SilentlyContinue }

$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random
$room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
$stem = [IO.Path]::GetFileNameWithoutExtension($Rom)

# Distinct, recognisable 32 KiB saves per player (slot 0 = 0xA0.., slot 1 = 0xB0..) so we can prove who loaded whose.
$procs = @()
foreach ($slot in 0, 1) {
    $sv = Join-Path $out ('sv' + $slot); New-Item -ItemType Directory -Force -Path $sv | Out-Null
    $bytes = New-Object byte[] 32768
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = (0xA0 + 0x10 * $slot + ($i % 7)) -band 0xFF }
    [IO.File]::WriteAllBytes((Join-Path $sv ($stem + '.srm')), $bytes)
    $log = Join-Path $out ('slot' + $slot + '.log')
    $a = @('--core', $core, '--rom', (Join-Path $root $Rom), '--system', 'gb', '--link', '--save-dir', $sv, '--state-dir', (Join-Path $out ('st' + $slot)),
        '--headless', '--frames', $Frames, '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $slot, '--players', 2,
        '--ports', '0,1,-1,-1', '--delay', 'auto', '--log', $log)
    if ($Rollback) { $a += '--rollback' }
    $procs += [pscustomobject]@{ Slot = $slot; Log = $log; Proc = (Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden) }
}
$t0 = Get-Date
while (@($procs | Where-Object { -not $_.Proc.HasExited }).Count -gt 0) {
    if (((Get-Date) - $t0).TotalSeconds -gt 600) { $procs | ForEach-Object { if (-not $_.Proc.HasExited) { $_.Proc.Kill() } }; Write-Output 'HARD TIMEOUT'; break }
    Start-Sleep -Milliseconds 500
}
$fin = @{}
$fail = 0
foreach ($p in $procs) {
    $txt = Get-Content $p.Log -Raw
    $m = [regex]::Match($txt, 'final: frame (\d+) state-hash (\w+) output-hash (\w+) input-hash (\w+)')
    $fin[$p.Slot] = $m.Groups
    Write-Output ("slot " + $p.Slot + " exit=" + $p.Proc.ExitCode + " " + $m.Value + " desync=" + ($txt -match 'DESYNC'))
    if ($p.Proc.ExitCode -ne 0 -or -not $m.Success -or $txt -match 'DESYNC') { $fail++ }
    # save exchange evidence
    ([regex]::Matches($txt, 'link: [^\r\n]*') | ForEach-Object { '  ' + $_.Value }) | Write-Output
}
if ($fin[0] -and $fin[1]) {
    foreach ($k in 2, 3, 4) { if ($fin[0][$k].Value -ne $fin[1][$k].Value) { Write-Output ("MISMATCH group " + $k); $fail++ } }
}
# each peer wrote back ONLY its own console's save (own file keeps its own marker byte; no .p2 file for the other player)
foreach ($slot in 0, 1) {
    $sv = Join-Path $out ('sv' + $slot)
    $b = [IO.File]::ReadAllBytes((Join-Path $sv ($stem + '.srm')))
    Write-Output ("slot " + $slot + " own save first byte 0x{0:X2} files: {1}" -f $b[0], ((Get-ChildItem $sv -Name) -join ','))
    if ((Get-ChildItem $sv -Name) -match '\.p2\.') { $fail++ }
}
if ($fail -eq 0) { Write-Output 'LINK QA PASS' } else { Write-Output ('LINK QA FAIL (' + $fail + ')'); exit 1 }
