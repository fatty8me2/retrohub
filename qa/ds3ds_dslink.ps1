# DS local wireless with a REAL game (no synthetic probe): N headless retrorun/melonDS instances on a private invisible desktop,
# joined through a local RetroHub room server in --ds-link mode. Proves both cores start wireless, the game boots on each,
# and reports link packet counters + a screenshot per peer. Muted, loopback only, kills only what it started.
# usage: qa\ds3ds_dslink.ps1 -Rom <plain .nds> [-Frames 2400] [-Server ws://127.0.0.1:9102] [-Players 2] [-Relay]
# -Fuzz        retrorun --fuzz-input (pseudo-random buttons, never Start/Select)
# PASS = every peer logs "link: wireless started", exits 0, no "connection lost" before the scripted end, and its final screenshot is not blank.
# Link traffic (rx>0 on both) is reported; it is only required with -RequireTraffic (a game must open a wireless session for that).
param(
    [Parameter(Mandatory = $true)][string]$Rom,
    [int]$Frames = 2400,
    [string]$Server = "ws://127.0.0.1:9102",
    [int]$Players = 2,
    [switch]$Relay, [switch]$Fuzz, [switch]$RequireTraffic,
    [string]$Out = "",
    [string]$Retrorun = "",
    [string]$Desktop = "rhds_link",
    [int]$TimeoutSec = 300
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
if ($Server -notmatch '^wss?://(127\.0\.0\.1|localhost)[:/]') { Write-Error "loopback servers only: $Server"; exit 2 }
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = if ($Retrorun) { $Retrorun } else { Join-Path $root "build\bin\Release\retrorun.exe" }
$core = Join-Path $root "deps\cores\melondsds_libretro.dll"
if (-not (Test-Path $exe)) { Write-Error "no retrorun at $exe"; exit 2 }
if (-not (Test-Path $core)) { Write-Error "no core at $core"; exit 2 }
if (-not (Test-Path $Rom)) { Write-Error "no rom $Rom"; exit 2 }
. "$PSScriptRoot\ds3ds_desk.ps1"
$tmp = if ($Out) { $Out } else { Join-Path $env:TEMP ("dsreal_" + [guid]::NewGuid().ToString("N").Substring(0, 8)) }
New-Item -ItemType Directory -Force $tmp | Out-Null
$code = -join ((1..6) | ForEach-Object { "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"[(Get-Random -Max 32)] })
$env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"
Remove-Item Env:RETRORUN_LINK_PROBE -ErrorAction SilentlyContinue
if ($Relay) { $env:HUBNET_FORCE_RELAY = "1" } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue; $env:HUBNET_LOOPBACK = "1" }
New-HiddenDesktop $Desktop
$procs = @()
for ($s = 0; $s -lt $Players; $s++) {
    $d = Join-Path $tmp "p$s"
    New-Item -ItemType Directory -Force $d | Out-Null
    $a = @("--core", $core, "--rom", $Rom, "--system", "nds", "--save-dir", $d, "--state-dir", $d, "--headless", "--frames", $Frames,
           "--net", $Server, "--room", "$code-1", "--slot", $s, "--players", $Players, "--ds-link", "--log", (Join-Path $d "retrorun.log"),
           "--screenshot", (Join-Path $d "final.png"))
    if ($Fuzz) { $a += "--fuzz-input" }
    $cmd = '"' + $exe + '" ' + (($a | ForEach-Object { if ("$_" -match "\s") { "`"$_`"" } else { "$_" } }) -join " ")
    $procs += [DeskLaunch]::Start($cmd, $d, $Desktop)
}
$deadline = (Get-Date).AddSeconds($TimeoutSec)
foreach ($p in $procs) { [DeskLaunch]::Wait($p, [Math]::Max(1000, [int](($deadline - (Get-Date)).TotalMilliseconds))) | Out-Null }
$ok = $true
for ($s = 0; $s -lt $Players; $s++) {
    $log = Join-Path $tmp "p$s\retrorun.log"
    $rc = [DeskLaunch]::ExitCode($procs[$s])
    "---- slot $s (exit $rc) ----"
    if (-not (Test-Path $log)) { "no log"; $ok = $false; continue }
    Get-Content $log | Select-String "link:|netpacket|done:|WARNING" | Select-Object -Last 14 | ForEach-Object { $_.Line }
    if (-not (Select-String -Path $log -Pattern "link: wireless started" -Quiet)) { "slot ${s}: wireless never started"; $ok = $false }
    $lines = @(Get-Content $log)
    $lost = [array]::IndexOf(@($lines | ForEach-Object { $_ -match 'connection lost' }), $true)
    $done = [array]::IndexOf(@($lines | ForEach-Object { $_ -match 'done: ' }), $true)
    if ($lost -ge 0 -and ($done -lt 0 -or $lost -lt $done)) { "slot ${s}: connection lost before the scripted end"; $ok = $false }
    $fin = Select-String -Path $log -Pattern "link: final tx (\d+) .* rx (\d+) " | Select-Object -Last 1
    if ($fin) {
        $tx = [int]$fin.Matches[0].Groups[1].Value; $rx = [int]$fin.Matches[0].Groups[2].Value
        "slot ${s}: wireless packets tx=$tx rx=$rx"
        if ($RequireTraffic -and ($tx -lt 1 -or $rx -lt 1)) { "slot ${s}: no wireless traffic"; $ok = $false }
    }
    if ($rc -ne 0) { $ok = $false }
    $png = Join-Path $tmp "p$s\final.png"
    if (-not (Test-Path $png)) { "slot ${s}: no screenshot"; $ok = $false }
    else {
        Add-Type -AssemblyName System.Drawing
        $bmp = New-Object System.Drawing.Bitmap $png
        $seen = @{}
        for ($y = 0; $y -lt $bmp.Height; $y += 4) { for ($x = 0; $x -lt $bmp.Width; $x += 4) { $seen[$bmp.GetPixel($x, $y).ToArgb()] = 1 } }
        $bmp.Dispose()
        "slot ${s}: screenshot $png distinct colours (sampled) = $($seen.Count)"
        if ($seen.Count -lt 8) { "slot ${s}: screenshot looks blank"; $ok = $false }
    }
}
"logs: $tmp"
if ($ok) { "RESULT: PASS" } else { "RESULT: FAIL"; exit 1 }
