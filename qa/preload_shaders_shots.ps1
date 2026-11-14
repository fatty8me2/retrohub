# Headless UI shots and Play traces for the "Shaders at start" Switch setting (switch.preload_shaders, Video group):
#   1. the per-game Settings tab of Ocarina of Time (Switch 2), default value (Auto), at each size, scrolled in steps
#   2. the Switch "Game defaults" area in Settings > Video & Games (settings:2), scrolled in steps
#   3. the per-game Settings tab with a stored "all" value (combo shows "All cached shaders")
#   4. the headless Play path (RETROHUB_AUTOTEST=play) for that game with a fake Lichii install: stored auto, all, recent.
#      The trace line "... | preload all shaders yes|no" is copied to <shot>.trace.txt. Auto is yes on a PC with 60 GiB or more of RAM.
#      The autotest log line "lichii: build_all_cached_shaders true|false, EDEN_NX2_NO_REACTIVE set|not set" names the fact written to Lichii's qt-config.ini
#      (the old env var EDEN_NX2_PRELOAD_ALL_PIPELINES is gone since Lichii build 160).
# Every run is hidden, muted, no network (HUB_SCREENSHOT mode) and killed by PID if it overruns. Nothing is written to -DataFrom.
# Nothing is started: the fake Lichii is a 2-byte MZ file, and a headless launch is skipped before anything spawns.
# Usage: qa\preload_shaders_shots.ps1 [-DataFrom <userdata dir>] [-Build <bin\Release>] [-Out <dir>] [-Sizes 1280x800,1600x1000] [-Secs 12]
param([string]$DataFrom = "C:\Users\fatty\Desktop\smashClassic\userdata", [string]$Build = "", [string]$Out = "",
      [string[]]$Sizes = @("1280x800", "1600x1000"), [int]$Secs = 12)
$ErrorActionPreference = "Stop"
$OOT = "switch:0400486024dec000"
# the pinned Lichii build (emulators.cpp, Kind::Lichii): a marker naming it and its sha256 means "not outdated"
$LICHII_SHA = "e6ac8c09ddd00cb64e539f2a85f9f9f32d579c772b3619aed0d4d3910a93e833"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { throw "no RetroHub.exe in $Build" }
$Build = (Resolve-Path $Build).Path
if (-not (Test-Path (Join-Path $DataFrom "library_cache.json"))) { throw "DataFrom has no library_cache.json: $DataFrom" }
if (-not $Out) { $Out = Join-Path $root "qa_out\preload-shaders" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Sizes = @($Sizes | ForEach-Object { "$_" -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing

# ---- an isolated copy of the library (small top-level files + art), a fake current Lichii install, optional vault game settings ----
function New-Copy([string]$tag, [string]$gameSettingsJson = "") {
    $run = Join-Path $env:TEMP ("rh_preload_" + $tag + "_" + [guid]::NewGuid().ToString("N").Substring(0, 6))
    New-Item -ItemType Directory -Force $run | Out-Null
    Copy-Item (Join-Path $Build "RetroHub.exe"), (Join-Path $Build "SDL2.dll") $run
    foreach ($d in "data", "controls") { if (Test-Path (Join-Path $Build $d)) { Copy-Item (Join-Path $Build $d) (Join-Path $run $d) -Recurse } }
    $ud = Join-Path $run "userdata"
    New-Item -ItemType Directory -Force $ud | Out-Null
    Get-ChildItem -LiteralPath $DataFrom -File | Where-Object { $_.Length -lt 5MB } | ForEach-Object { Copy-Item -LiteralPath $_.FullName $ud }
    & robocopy (Join-Path $DataFrom "art") (Join-Path $ud "art") /E /NFL /NDL /NJH /NJS /NP | Out-Null
    # the vault lives inside the copy: Settings values are read from and written to it, never the real vault
    $vault = Join-Path $ud "vault"
    $hubPath = Join-Path $ud "hub.json"
    $hubText = [IO.File]::ReadAllText($hubPath)
    $hubText = [regex]::Replace($hubText, '"vault_dir"\s*:\s*"[^"]*"', ('"vault_dir": "' + $vault.Replace('\', '\\') + '"'))
    [IO.File]::WriteAllText($hubPath, $hubText, (New-Object System.Text.UTF8Encoding $false))
    # a complete managed Lichii at the pinned build (marker written last, as the installer does). Only the marker matters; the exe is never run.
    $l = Join-Path $ud "emulators\lichii"
    New-Item -ItemType Directory -Force $l | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $l "lichii.exe"), [byte[]]@(0x4D, 0x5A))
    [IO.File]::WriteAllText((Join-Path $l "retrohub-install.txt"), "Lichii build 177`nsha256 $LICHII_SHA`n", (New-Object System.Text.UTF8Encoding $false))
    if ($gameSettingsJson) {
        New-Item -ItemType Directory -Force (Join-Path $vault "_settings") | Out-Null
        [IO.File]::WriteAllText((Join-Path $vault "_settings\game_settings.json"), $gameSettingsJson, (New-Object System.Text.UTF8Encoding $false))
    }
    return $run
}

# ---- one hidden, muted, headless RetroHub run; killed by PID on overrun ----
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
    if (Test-Path $tr) {
        Copy-Item $tr (Join-Path $Out "$name.trace.txt") -Force
        Write-Host "  trace:"
        Get-Content $tr | Where-Object { $_ -match '^(external|notice|hero)' } | ForEach-Object { Write-Host "    $_" }
    }
}

# A shot; $ticks > 0 scrolls the modal / page down (HUB_SHOT_WHEEL=x,y,ticks) at 50% width and 60% height. 0 = the top.
function Shot([string]$name, [string]$run, [string]$size, [hashtable]$extra, [int]$ticks = 0) {
    $e = @{ HUB_SHOT_SIZE = $size }
    foreach ($k in $extra.Keys) { $e[$k] = $extra[$k] }
    if ($ticks -gt 0 -and $size -match '^(\d+)x(\d+)$') {
        $e["HUB_SHOT_WHEEL"] = ("{0},{1},{2}" -f [int]([int]$Matches[1] * 0.5), [int]([int]$Matches[2] * 0.6), $ticks)
    }
    Invoke-Run $run $e
    Save-Result $run "$name-$size"
}

$autoCopyJson = ""
$allJson = '{"version": "1", "systems": {}, "games": {"' + $OOT + '": {"switch.preload_shaders": "all"}}}'
$recentJson = '{"version": "1", "systems": {}, "games": {"' + $OOT + '": {"switch.preload_shaders": "recent"}}}'
$copies = @()
try {
    $auto = New-Copy "au" $autoCopyJson;  $copies += $auto
    $all = New-Copy "al" $allJson;        $copies += $all
    $recent = New-Copy "rc" $recentJson;  $copies += $recent
    Write-Host "copies: $auto | $all | $recent"

    foreach ($s in $Sizes) {
        # 1. per-game Settings tab (tab 3), default value: top, then scrolled in steps
        # the Graphics group (with "Shaders at start" last) is only the first few wheel ticks; 6 ticks already reach the bottom of the tab
        foreach ($t in 0, 1, 2, 3, 4) {
            Shot "game-settings-auto-scroll$t" $auto $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "3" } $t
        }
        # 2. Switch "Game defaults" in Settings > Video & Games (settings:2). HUB_SHOT_GS_SYS=12 selects Switch in the defaults combo
        # (index into the sysl list in settings_games_ui.cpp); the page is scrolled to the defaults area.
        foreach ($t in 0, 28, 40, 60) {
            Shot "settings-defaults-switch-scroll$t" $auto $s @{ HUB_SHOT_PAGE = "settings:2"; HUB_SHOT_GS_SYS = "12" } $t
        }
        # 3. a stored "all" value: the combo shows "All cached shaders"
        Shot "game-settings-all" $all $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "3" } 3
    }

    # 4. headless Play: RETROHUB_AUTOTEST=play presses the Play code path (play_entry -> launch_external) for Ocarina.
    # Headless launches are skipped; the trace line "external ... | preload all shaders yes|no" says what the launch would write ([Renderer] build_all_cached_shaders).
    $playEnv = @{ RETROHUB_AUTOTEST = "play"; RETROHUB_AUTOTEST_GAME = $OOT; HUB_SHOT_SECS = "25"; HUB_SHOT_SIZE = $Sizes[0] }
    foreach ($pair in @(@("play-auto", $auto), @("play-all", $all), @("play-recent", $recent))) {
        Invoke-Run $pair[1] $playEnv
        Save-Result $pair[1] ($pair[0] + "-" + $Sizes[0])
    }
}
finally {
    foreach ($c in $copies) { Remove-Item -LiteralPath $c -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host "screenshots in $Out"
exit 0
