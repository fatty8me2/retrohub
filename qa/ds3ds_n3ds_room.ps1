# 3DS (Azahar) two-peer connect through RetroHub's real room flow, fully automated and invisible. Wraps qa\n3ds_room_online.ps1:
#   1. managed Azahar copy: extracts the PINNED release zip (SHA-256 checked; nothing of the user's own install is read or touched),
#   2. seeds TWO independent Azahar user dirs by booting the game once offline on a hidden desktop (this gives each peer its own
#      random Console ID + MAC; copied dirs collide: "Your Console ID conflicted with someone else's in the room"),
#   3. runs host + guest RetroHub (RETROHUB_AUTOTEST) against a LOCAL worker and checks azahar-room.out + the join driver output.
# PASS = host room log shows BOTH 3DS nicknames "has joined" and the guest join driver prints "joined".
# usage: qa\ds3ds_n3ds_room.ps1 -AzaharZip <azahar-windows-msvc-*.zip> -Rom <decrypted .3ds/.cci (NOT a zip)> [-Work <scratch>]
#        [-Server ws://127.0.0.1:9102] [-Seconds 100] [-SeedSeconds 40] [-Relay] [-Game pokex] [-KeepSeeds]
# Needs: build\bin\Release\RetroHub.exe (+ the exes next to it). The rom must be decrypted (NoCrypto) or Azahar needs aes_keys.txt.
param(
    [Parameter(Mandatory)][string]$AzaharZip, [Parameter(Mandatory)][string]$Rom,
    [string]$Work = "", [string]$Server = "ws://127.0.0.1:9102", [int]$Seconds = 100, [int]$SeedSeconds = 40,
    [string]$Game = "pokex", [switch]$Relay, [switch]$KeepSeeds,
    [string]$Sha256 = "4412eac8d13123c8840f11890fc26e5b0e4c76d16aa28a608941377b948a9db0"
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
if ($Server -notmatch '^wss?://(127\.0\.0\.1|localhost)[:/]') { Write-Error "loopback servers only: $Server"; exit 2 }
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\ds3ds_desk.ps1"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build\bin\Release"
if (-not (Test-Path "$build\RetroHub.exe")) { Write-Error "no RetroHub.exe in $build"; exit 2 }
if ((Get-FileHash $AzaharZip -Algorithm SHA256).Hash -ne $Sha256.ToUpper()) { Write-Error "Azahar zip SHA-256 does not match the pinned release"; exit 2 }
if ($Rom -match '\.zip$' -or -not (Test-Path $Rom)) { Write-Error "-Rom must be an existing unzipped .3ds/.cci"; exit 2 }
if (-not $Work) { $Work = Join-Path $env:TEMP ("n3dsroom_" + [guid]::NewGuid().ToString("N").Substring(0, 6)) }
New-Item -ItemType Directory -Force $Work | Out-Null
$az = Join-Path $Work "azclean"
if (-not (Test-Path "$az\azahar.exe")) {
    Expand-Archive $AzaharZip "$Work\azx" -Force
    $inner = Get-ChildItem "$Work\azx" -Directory | Select-Object -First 1
    Move-Item $inner.FullName $az
}
$romdir = Join-Path $Work "rom"
New-Item -ItemType Directory -Force $romdir | Out-Null
if (-not (Test-Path "$romdir\$(Split-Path $Rom -Leaf)")) { New-Item -ItemType HardLink -Path "$romdir\$(Split-Path $Rom -Leaf)" -Target $Rom -ErrorAction SilentlyContinue | Out-Null }
if (-not (Test-Path "$romdir\$(Split-Path $Rom -Leaf)")) { Copy-Item $Rom $romdir }

$ini = @"
[Audio]
output_type\default=false
output_type=1
volume\default=false
volume=0

[UI]
confirmClose\default=false
confirmClose=false
check_for_update_on_start\default=false
check_for_update_on_start=false
Updater\check_for_update_on_start\default=false
Updater\check_for_update_on_start=false
firstStart\default=false
firstStart=false
showConsole\default=true
showConsole=false
"@
New-HiddenDesktop "rhn3_seed"
$env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"
$users = @()
foreach ($n in "A", "B") {
    $seed = Join-Path $Work "seed$n"
    $u = Join-Path $seed "user"
    if ((Test-Path "$u\nand\data\00000000000000000000000000000000\sysdata\00010017\00000000\config") -and $KeepSeeds) { $users += $u; continue }
    Remove-Item $seed -Recurse -Force -ErrorAction SilentlyContinue
    robocopy $az "$seed\az" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    New-Item -ItemType Directory -Force "$seed\az\user\config" | Out-Null
    Set-Content "$seed\az\user\config\qt-config.ini" $ini -Encoding ascii
    $h = [DeskLaunch]::Start("`"$seed\az\azahar.exe`" -f `"$romdir\$(Split-Path $Rom -Leaf)`"", "$seed\az", "rhn3_seed")
    $pidSeed = (Get-Process azahar -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$seed\az\azahar.exe" } | Select-Object -First 1).Id
    Start-Sleep $SeedSeconds
    foreach ($p in Get-Process azahar -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$seed\az\azahar.exe" }) { Stop-Process -Id $p.Id -Force }
    Start-Sleep 2
    robocopy "$seed\az\user" $u /E /NFL /NDL /NJH /NJS /NP | Out-Null
    $cfg = "$u\nand\data\00000000000000000000000000000000\sysdata\00010017\00000000\config"
    if (-not (Test-Path $cfg)) { Write-Error "seed $n failed: Azahar did not create its 3DS settings ($cfg). Raise -SeedSeconds or check the rom (decrypted?)."; exit 1 }
    "seed ${n}: console config created, mac $((Get-Content "$u\sysdata\mac.txt" -ErrorAction SilentlyContinue))"
    $users += $u
}
$macA = Get-Content "$($users[0])\sysdata\mac.txt" -ErrorAction SilentlyContinue
$macB = Get-Content "$($users[1])\sysdata\mac.txt" -ErrorAction SilentlyContinue
if ($macA -and $macA -eq $macB) { Write-Warning "both seeds have the same MAC ($macA); the room may reject the second peer" }

$code = -join ((1..6) | ForEach-Object { "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"[(Get-Random -Max 32)] })   # server room alphabet excludes I/O/0/1
$args2 = @("-NoProfile", "-File", "$PSScriptRoot\n3ds_room_online.ps1", "-Code", $code,"-Build", $build, "-AzaharDir", $az, "-UserA", $users[0], "-UserB", $users[1],
           "-Work", "$Work\run", "-RomDir", $romdir, "-Game", $Game, "-Seconds", $Seconds, "-Server", $Server, "-Desktop", "rhn3_room")
if ($Relay) { $args2 += "-Relay" }
$out = & powershell @args2 2>&1 | ForEach-Object { "$_" }
$out | ForEach-Object { $_ }
$txt = $out -join "`n"
$hostJoin = ([regex]::Matches($txt, "has joined")).Count
$guestJoined = ([regex]::Matches($txt, "(?m)^joined\s*$")).Count -ge 2
$conflict = $txt -match "Console ID conflicted"
"---- summary ----"
"room 'has joined' lines: $hostJoin; both join drivers reached 'joined': $guestJoined; console-id conflict: $conflict"
"work dir: $Work"
if ($hostJoin -ge 2 -and ($guestJoined)) { "RESULT: PASS" } else { "RESULT: FAIL"; exit 1 }
