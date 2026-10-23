# Two headless retrorun instances of one DS game joined through a RetroHub room in DS link (local wireless) mode.
# Headless: no windows, muted. Passes when both cores start wireless and exchange link traffic.
# Usage: qa\ds_link.ps1 [-Rom <nds or zip>] [-Frames 1500] [-Server wss://...] [-Players 2] [-Relay]
param(
    [Parameter(Mandatory = $true)][string]$Rom,
    [int]$Frames = 1500,
    [string]$Server = "wss://smash-netplay.jumpcircuit.workers.dev",
    [int]$Players = 2,
    [switch]$Relay
)
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\bin\Release\retrorun.exe"
$core = Join-Path $root "deps\cores\melondsds_libretro.dll"
$tmp = Join-Path $env:TEMP ("dslink_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
$code = -join ((1..6) | ForEach-Object { "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"[(Get-Random -Max 32)] })
$env:RETRORUN_MUTE = "1"
$env:RETRORUN_LINK_PROBE = "1"
if ($Relay) { $env:HUBNET_FORCE_RELAY = "1" }
$procs = @()
for ($s = 0; $s -lt $Players; $s++) {
    $d = Join-Path $tmp "p$s"
    New-Item -ItemType Directory -Force $d | Out-Null
    $log = Join-Path $d "retrorun.log"
    $a = @("--core", $core, "--rom", $Rom, "--system", "nds", "--save-dir", $d, "--state-dir", $d, "--headless", "--frames", $Frames,
           "--net", $Server, "--room", "$code-1", "--slot", $s, "--players", $Players, "--ds-link", "--log", $log)
    $procs += Start-Process -FilePath $exe -ArgumentList ($a | ForEach-Object { if ("$_" -match "\s") { "`"$_`"" } else { "$_" } }) -WindowStyle Hidden -PassThru
}
$procs | ForEach-Object { $_.WaitForExit(240000) | Out-Null }
$ok = $true
for ($s = 0; $s -lt $Players; $s++) {
    $log = Join-Path $tmp "p$s\retrorun.log"
    "---- slot $s (exit $($procs[$s].ExitCode)) ----"
    Get-Content $log | Select-String "link:|multiplayer|netpacket|done:" | Select-Object -Last 12 | ForEach-Object { $_.Line }
    if (-not (Select-String -Path $log -Pattern "link: wireless started" -Quiet)) { $ok = $false }
    $pl = Select-String -Path $log -Pattern "PROBE tx (\d+) rx (\d+) corrupt (\d+)" | Select-Object -Last 1
    if (-not $pl -or [int]$pl.Matches[0].Groups[2].Value -lt 10 -or [int]$pl.Matches[0].Groups[3].Value -ne 0) { $ok = $false }
    if ($procs[$s].ExitCode -ne 0) { $ok = $false }
}
"logs: $tmp"
if ($ok) { "RESULT: PASS" } else { "RESULT: FAIL"; exit 1 }
