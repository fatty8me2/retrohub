# Headless UI shots for the "Lichii is the default again + Lichii build 95" change, the parts hookshot_ui_shots.ps1 does not cover:
#   1. the Hookshot card in every state (idle, problems, building, paused, failed, done = Play uses Lichii, done_on = Play uses Hookshot)
#   2. the "newer Lichii" offer at Play (HUB_SHOT_EMU=lichiiupdate)
#   3. Settings > Video & Games with an OUTDATED managed Lichii (build 88 on disk, the pinned build offered): the newer-version text and the Update button.
#      The fake install (lichii.exe, any bytes, and retrohub-install.txt naming build-88) lives only in the run's own copied data folder. Nothing is started.
# Every run is hidden, muted, no network (HUB_SCREENSHOT mode) and killed by PID if it overruns. Nothing is written to -DataFrom.
# Usage: qa\lichii_update_shots.ps1 [-DataFrom <userdata dir>] [-Build <bin\Release>] [-Out <dir>] [-Sizes 1280x800,1600x1000] [-Secs 12]
# The per-game "Plays with" row with the old switch.runner "auto" is shot by hookshot_ui_shots.ps1 (settings-ready-auto).
param([string]$DataFrom = "C:\Users\fatty\Desktop\smashClassic\userdata", [string]$Build = "", [string]$Out = "",
      [string[]]$Sizes = @("1280x800", "1600x1000"), [int]$Secs = 12)
$ErrorActionPreference = "Stop"
$OOT = "switch:0400486024dec000"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { throw "no RetroHub.exe in $Build" }
$Build = (Resolve-Path $Build).Path
if (-not (Test-Path (Join-Path $DataFrom "library_cache.json"))) { throw "DataFrom has no library_cache.json: $DataFrom" }
if (-not $Out) { $Out = Join-Path $root "qa_out\lichii-update" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Sizes = @($Sizes | ForEach-Object { "$_" -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing

# ---- an isolated copy of the library (small top-level files + art), optionally with a fake Hookshot build, a fake OUTDATED Lichii, a vault settings file ----
function New-Copy([string]$tag, [bool]$hookshotBuild, [string]$gameSettingsJson = "", [bool]$oldLichii = $false) {
    $run = Join-Path $env:TEMP ("rh_lcupd_" + $tag + "_" + [guid]::NewGuid().ToString("N").Substring(0, 6))
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
    if ($hookshotBuild) {
        $h = Join-Path $ud "emulators\hookshot"
        New-Item -ItemType Directory -Force (Join-Path $h "romfs") | Out-Null
        foreach ($f in "hookshot.exe", "hookshot_vk.dll", "SDL2.dll") { [IO.File]::WriteAllBytes((Join-Path $h $f), [byte[]]@()) }
        [IO.File]::WriteAllText((Join-Path $h "retrohub_hookshot.json"), '{"commit":"8ef84611049f4fc31835a0b12793cf074ed49c3c","game_build":"e0f3ccfa","built":1794066770,"source":"x","size":1}')
    }
    if ($oldLichii) {
        # a complete managed install of build 88 (marker written last, as the installer does). Only the marker text matters; the exe is never run.
        $l = Join-Path $ud "emulators\lichii"
        New-Item -ItemType Directory -Force $l | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $l "lichii.exe"), [byte[]]@(0x4D, 0x5A))
        [IO.File]::WriteAllText((Join-Path $l "retrohub-install.txt"), "Lichii build-88`nsha256 3405860a13f7b0729548ddb9402a355f4cfb1cd6a6d705fb8ea26f88879b0407`n", (New-Object System.Text.UTF8Encoding $false))
    }
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
    if (Test-Path $tr) { Copy-Item $tr (Join-Path $Out "$name.trace.txt") -Force }
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

$hookshotJson = '{"version": "1", "systems": {}, "games": {"' + $OOT + '": {"switch.runner": "hookshot"}}}'
$lichiiJson = '{"version": "1", "systems": {}, "games": {"' + $OOT + '": {"switch.runner": "lichii"}}}'
$copies = @()
try {
    $notbuilt = New-Copy "nb" $false;                   $copies += $notbuilt
    $lichii = New-Copy "lc" $true $lichiiJson;          $copies += $lichii
    $hookshot = New-Copy "hk" $true $hookshotJson;      $copies += $hookshot
    $outdated = New-Copy "old" $false "" $true;         $copies += $outdated
    Write-Host "copies: $notbuilt | $lichii | $hookshot | $outdated"

    foreach ($s in $Sizes) {
        # 1. Hookshot card states (About tab, the card is at the end, so scroll it into view)
        foreach ($m in "idle", "problems", "building", "paused", "failed") {
            Shot "card-$m" $notbuilt $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "0"; HUB_SHOT_HOOKSHOT = $m } 30
        }
        Shot "card-done" $lichii $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "0"; HUB_SHOT_HOOKSHOT = "done" } 30
        Shot "card-done_on" $hookshot $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "0"; HUB_SHOT_HOOKSHOT = "done_on" } 30

        # 2. the newer-Lichii offer at Play
        Shot "emu-lichiiupdate" $notbuilt $s @{ HUB_SHOT_EMU = "lichiiupdate" }

        # 3. Settings > Video & Games (tab 2) with the OUTDATED managed Lichii: top, then scrolled to the Switch block
        foreach ($t in 0, 22, 28, 40) {
            Shot "settings-outdated-lichii-scroll$t" $outdated $s @{ HUB_SHOT_PAGE = "settings:2" } $t
        }
    }
}
finally {
    foreach ($c in $copies) { Remove-Item -LiteralPath $c -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host "screenshots in $Out"
exit 0
