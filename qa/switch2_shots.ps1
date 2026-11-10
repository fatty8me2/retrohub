# Headless UI shots: Nintendo Switch and Nintendo Switch 2 are two distinct consoles. Home hero of a Switch 2 game (Ocarina of Time), the All Games console
# chips (Switch + Switch 2) and the Switch 2 list filter, the Switch 2 game page header, Settings > Video & Games > Game defaults (per-console list, Switch 2
# inherits Switch), the Controllers page Switch 2 tab. HUB_SHOT_SWITCH=2 seeds fake Switch games incl. Ocarina of Time (a Switch 2 game).
# Every run is hidden, muted, no network (HUB_SCREENSHOT mode) and killed by PID if it overruns. The library is copied (small top-level files + art) into a temp folder; -DataFrom is only read.
# Usage: qa\fritchii_shots.ps1 [-DataFrom <userdata dir>] [-Build <bin\Release>] [-Out <dir>] [-Sizes 1280x800] [-Secs 12]
param([string]$DataFrom = "C:\Users\fatty\Desktop\smashClassic\userdata", [string]$Build = "", [string]$Out = "",
      [string[]]$Sizes = @("1280x800"), [int]$Secs = 12, [string]$Only = "")
$ErrorActionPreference = "Stop"
$OOT = "switch:0400486024dec000"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { throw "no RetroHub.exe in $Build" }
$Build = (Resolve-Path $Build).Path
if (-not (Test-Path (Join-Path $DataFrom "library_cache.json"))) { throw "DataFrom has no library_cache.json: $DataFrom" }
if (-not $Out) { $Out = Join-Path $root "qa_out\switch2" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Sizes = @($Sizes | ForEach-Object { "$_" -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing
$FakeSha = "068fb4c66b2e32447ac7690393039043854de4b189aa7e95b34c4bf0d2a4080e"   # the real Fritchii v4.1 pin: a current install

function New-Copy([string]$tag, [bool]$lichii, [bool]$fritchii, [string]$gameSettingsJson = "") {
    $run = Join-Path $env:TEMP ("rh_sw2_" + $tag + "_" + [guid]::NewGuid().ToString("N").Substring(0, 6))
    New-Item -ItemType Directory -Force $run | Out-Null
    Copy-Item (Join-Path $Build "RetroHub.exe"), (Join-Path $Build "SDL2.dll") $run
    foreach ($d in "data", "controls") { if (Test-Path (Join-Path $Build $d)) { Copy-Item (Join-Path $Build $d) (Join-Path $run $d) -Recurse } }
    $ud = Join-Path $run "userdata"
    New-Item -ItemType Directory -Force $ud | Out-Null
    Get-ChildItem -LiteralPath $DataFrom -File | Where-Object { $_.Length -lt 5MB } | ForEach-Object { Copy-Item -LiteralPath $_.FullName $ud }
    & robocopy (Join-Path $DataFrom "art") (Join-Path $ud "art") /E /NFL /NDL /NJH /NJS /NP | Out-Null
    $vault = Join-Path $ud "vault"
    $hubPath = Join-Path $ud "hub.json"
    $hubText = [IO.File]::ReadAllText($hubPath)
    $hubText = [regex]::Replace($hubText, '"vault_dir"\s*:\s*"[^"]*"', ('"vault_dir": "' + $vault.Replace('\', '\') + '"'))
    [IO.File]::WriteAllText($hubPath, $hubText, (New-Object System.Text.UTF8Encoding $false))
    $lsha = "f09893b3041596662d762959a103ae4af40d479631309fff9990bcc288078eed"   # Lichii build 130's pin: a current install
    if ($lichii) {
        $l = Join-Path $ud "emulators\lichii"
        New-Item -ItemType Directory -Force $l | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $l "lichii.exe"), [byte[]]@(0x4D, 0x5A))
        [IO.File]::WriteAllText((Join-Path $l "retrohub-install.txt"), "Lichii build 130`nsha256 $lsha`n", (New-Object System.Text.UTF8Encoding $false))
    }
    if ($fritchii) {
        $f = Join-Path $ud "emulators\fritchii"
        New-Item -ItemType Directory -Force $f | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $f "lichii.exe"), [byte[]]@(0x4D, 0x5A))
        [IO.File]::WriteAllText((Join-Path $f "retrohub-install.txt"), "Fritchii v4.1`nsha256 $FakeSha`n", (New-Object System.Text.UTF8Encoding $false))
    }
    if ($gameSettingsJson) {
        New-Item -ItemType Directory -Force (Join-Path $vault "_settings") | Out-Null
        [IO.File]::WriteAllText((Join-Path $vault "_settings\game_settings.json"), $gameSettingsJson, (New-Object System.Text.UTF8Encoding $false))
    }
    return $run
}

function Invoke-Run([string]$run, [hashtable]$extra, [int]$timeoutSec = 150) {
    $bmp = Join-Path $run "shot.bmp"
    Remove-Item -LiteralPath $bmp, (Join-Path $run "shot_trace.txt") -ErrorAction SilentlyContinue
    $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_TRACE = (Join-Path $run "shot_trace.txt"); HUB_SHOT_SECS = "$Secs"; HUB_SHOT_VAULT = (Join-Path $run "userdata\vault") }
    foreach ($k in $extra.Keys) { $set[$k] = [string]$extra[$k] }
    try {
        foreach ($k in $set.Keys) { Set-Item -Path "env:$k" -Value $set[$k] }
        $p = Start-Process (Join-Path $run "RetroHub.exe") -WorkingDirectory $run -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit($timeoutSec * 1000)) { Stop-Process -Id $p.Id -Force; Write-Host "  overran: killed by PID $($p.Id)" }
    } finally {
        foreach ($k in $set.Keys) { Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue }
    }
}

function Save-Result([string]$run, [string]$name) {
    $bmp = Join-Path $run "shot.bmp"
    if (Test-Path $bmp) {
        $img = [System.Drawing.Image]::FromFile($bmp)
        $png = Join-Path $Out "$name.png"
        $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Write-Host "shot $png"
    } else { Write-Host "NO SHOT: $name" }
    $tr = Join-Path $run "shot_trace.txt"
    if (Test-Path $tr) { Copy-Item $tr (Join-Path $Out "$name.trace.txt") -Force }
}

function Shot([string]$name, [string]$run, [string]$size, [hashtable]$extra, [int]$ticks = 0) {
    if ($Only -and $name -notmatch $Only) { return }
    $e = @{ HUB_SHOT_SIZE = $size }
    foreach ($k in $extra.Keys) { $e[$k] = $extra[$k] }
    if ($ticks -gt 0 -and $size -match '^(\d+)x(\d+)$') {
        $e["HUB_SHOT_WHEEL"] = ("{0},{1},{2}" -f [int]([int]$Matches[1] * 0.5), [int]([int]$Matches[2] * 0.6), $ticks)
    }
    Invoke-Run $run $e
    Save-Result $run "$name-$size"
}

$OOTKEY = $OOT
$copies = @()
try {
    $run = New-Copy "s2" $false $false; $copies += $run
    Write-Host "copy: $run"
    $base = @{ HUB_SHOT_FAKE_LIB = "2"; HUB_SHOT_SWITCH = "2" }
    function M([hashtable]$x) { $h = @{} + $base; foreach ($k in $x.Keys) { $h[$k] = $x[$k] }; return $h }
    foreach ($s in $Sizes) {
        Shot "home-hero-switch2" $run $s (M @{ HUB_SELECT_GAME = $OOTKEY }) 
        Shot "home-recently-played" $run $s (@{ HUB_SHOT_FAKE_LIB = "12"; HUB_SHOT_CONTINUE = "1"; HUB_SHOT_PLAYTIME = "1" }) 6
        Shot "home-order-scroll" $run $s (M @{ HUB_SELECT_GAME = $OOTKEY; HUB_SHOT_CONTINUE = "1" }) 8
        Shot "home-hero-switch" $run $s (M @{ HUB_SELECT_GAME = "switch:01007ef00011e000" })
        Shot "library-chips-grid" $run $s (M @{ HUB_START_VIEW = "all"; HUB_SHOT_LIBVIEW = "grid" })
        Shot "library-chips-list" $run $s (M @{ HUB_START_VIEW = "all"; HUB_SHOT_LIBVIEW = "list" })
        Shot "library-filter-switch2" $run $s (M @{ HUB_START_VIEW = "all"; HUB_SHOT_LIBVIEW = "grid"; HUB_START_SYS = "switch2" })
        Shot "game-page-switch2" $run $s (M @{ HUB_SHOT_DETAILS = $OOTKEY; HUB_SHOT_TAB = "0" }) 12
        Shot "game-page-switch2-settings" $run $s (M @{ HUB_SHOT_DETAILS = $OOTKEY; HUB_SHOT_TAB = "3" }) 12
        Shot "game-page-switch" $run $s (M @{ HUB_SHOT_DETAILS = "switch:01007ef00011e000"; HUB_SHOT_TAB = "0" }) 12
        foreach ($t in 12, 16, 20, 24) { Shot "settings-game-defaults-switch2-scroll$t" $run $s (M @{ HUB_SHOT_PAGE = "settings:2"; HUB_SHOT_GS_SYS = "13" }) $t }
        Shot "controllers-switch2" $run $s (M @{ HUB_SHOT_CTL_SYS = "switch2"; HUB_SHOT_FAKE_PADS = "1" })
        Shot "controllers-switch" $run $s (M @{ HUB_SHOT_CTL_SYS = "switch"; HUB_SHOT_FAKE_PADS = "1" })
    }
}
finally {
    foreach ($c in $copies) { Remove-Item -LiteralPath $c -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host "screenshots in $Out"
exit 0
