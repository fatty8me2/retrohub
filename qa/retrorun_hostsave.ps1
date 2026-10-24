# Headless host-save sharing QA for retrorun lockstep (Windows PowerShell 5.1).
# Two peers boot with DIFFERENT local battery saves. Without -Share the emulation diverges (state hash mismatch / DESYNC);
# with -Share the host's save + clock is sent to the guest before frame 0 and both finish with identical hashes.
# Usage: powershell -File qa\retrorun_hostsave.ps1 -Server ws://127.0.0.1:8797 [-Share] [-Relay] -Rom "<path to .gba/.gb/.zip>" -System gba -Core mgba_libretro.dll
param(
    [string]$Server = 'ws://127.0.0.1:8797',
    [string]$Rom = '',
    [string]$System = 'gba',
    [string]$Core = 'mgba_libretro.dll',
    [int]$SaveSize = 131072,
    [int]$Frames = 2400,
    [switch]$Share,
    [switch]$Relay,
    [string]$Tag = 'hostsave'
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$corePath = Join-Path $root ('deps\cores\' + $Core)
$out = Join-Path $root ('qa\out\' + $Tag + $(if ($Share) { '_share' } else { '_noshare' }) + $(if ($Relay) { '_relay' } else { '' }))
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:RETRORUN_MUTE = '1'
if ($Relay) { $env:HUBNET_FORCE_RELAY = '1' } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }

$romCopy = Join-Path $out ([IO.Path]::GetFileName($Rom))
Copy-Item -LiteralPath $Rom -Destination $romCopy
if ($romCopy -like '*.zip') {   # the hub unpacks archives; the core wants the raw ROM
    Expand-Archive -LiteralPath $romCopy -DestinationPath (Join-Path $out 'rom') -Force
    $romCopy = (Get-ChildItem (Join-Path $out 'rom') -File | Select-Object -First 1).FullName
}
$stem = [IO.Path]::GetFileNameWithoutExtension($romCopy)
$sv = @((Join-Path $out 'sv0'), (Join-Path $out 'sv1'))
$sizes = @{}
for ($i = 0; $i -lt 2; $i++) {
    New-Item -ItemType Directory -Force -Path $sv[$i] | Out-Null
    $b = New-Object byte[] $SaveSize
    (New-Object System.Random (1000 + $i)).NextBytes($b)
    [IO.File]::WriteAllBytes((Join-Path $sv[$i] ($stem + '.srm')), $b)
}
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'
$rnd = New-Object System.Random
$room = (-join (1..6 | ForEach-Object { $alpha[$rnd.Next($alpha.Length)] })) + '-1'
$procs = @()
foreach ($slot in 0, 1) {
    $st = Join-Path $out ('st' + $slot)
    New-Item -ItemType Directory -Force -Path $st | Out-Null
    $log = Join-Path $out ('slot' + $slot + '.log')
    $a = @('--core', $corePath, '--rom', ('"' + $romCopy + '"'), '--system', $System, '--save-dir', $sv[$slot], '--state-dir', $st, '--headless',
        '--frames', $Frames, '--fuzz-input', '--net', $Server, '--room', $room, '--slot', $slot, '--players', 2, '--ports', '0,1,-1,-1',
        '--delay', 'auto', '--log', $log)
    if ($Share) { $a += '--share-host-save' }
    $procs += Start-Process -FilePath $exe -ArgumentList $a -PassThru -WindowStyle Hidden
}
$t0 = Get-Date
foreach ($p in $procs) {
    $left = 300 - ((Get-Date) - $t0).TotalSeconds
    if ($left -lt 1) { $left = 1 }
    if (-not $p.WaitForExit([int]($left * 1000))) { $p.Kill(); Write-Output "HARD TIMEOUT pid $($p.Id)" }
}
$finals = @(); $desync = @()
foreach ($slot in 0, 1) {
    $txt = Get-Content (Join-Path $out ('slot' + $slot + '.log')) -Raw
    $m = [regex]::Match($txt, 'final: frame (\d+) state-hash ([0-9a-f]+) output-hash ([0-9a-f]+) input-hash ([0-9a-f]+)')
    $finals += $m.Value.Substring(0, [Math]::Min(120, $m.Value.Length))
    $desync += ([regex]::Matches($txt, 'DESYNC')).Count
    ($txt -split "`n") | Where-Object { $_ -match 'hostsave|save: ' } | ForEach-Object { Write-Output ("slot$slot " + $_.Trim()) }
}
Write-Output ("room=$room share=$Share relay=$Relay exit=" + (($procs | ForEach-Object { $_.ExitCode }) -join ','))
Write-Output ("slot0 " + $finals[0]); Write-Output ("slot1 " + $finals[1]); Write-Output ("DESYNC mentions: " + ($desync -join ','))
for ($i = 0; $i -lt 2; $i++) {
    $f = Join-Path $sv[$i] ($stem + '.srm')
    Write-Output ("sv$i srm sha1=" + (Get-FileHash -Algorithm SHA1 $f).Hash.Substring(0, 12) + " size=" + (Get-Item $f).Length)
}
$ok = ($finals[0] -ne '') -and ($finals[0] -eq $finals[1])
Write-Output ("RESULT: " + $(if ($ok) { 'IDENTICAL final hashes' } else { 'MISMATCH' }))
