# Headless UI shots of the Hookshot card (game page > About), the "Plays with" setting (game page > Settings), the headless Play path
# (its trace line) and the AV1 prompt. Every run is hidden, muted, no network (HUB_SCREENSHOT mode) and killed by PID if it overruns.
# Usage: qa\hookshot_ui_shots.ps1 [-DataFrom <userdata dir>] [-Build <bin\Release>] [-Out <dir>] [-Sizes 1280x800,1600x1000] [-Secs 12]
#   -DataFrom  the real userdata. Only its small top-level files and art\ are COPIED into a temp run folder: never emulators\ (Hookshot is
#              ~30 GB), rom-cache, saves, logs. Nothing is written to -DataFrom.
#   -Build     bin\Release of a built RetroHub (default: this worktree's build, else ..\hookshot's). Only RetroHub.exe + SDL2.dll + data\ + controls\ run.
#   -Out       PNGs, plus <shot>.trace.txt (HUB_SHOT_TRACE) and <shot>.autotest.log where the run wrote them (default: qa_out\hookshot-ui).
# States: "notbuilt" = the real library, no Hookshot build. "ready" = the same copy plus a FAKE build in userdata\emulators\hookshot
#   (empty hookshot.exe, hookshot_vk.dll, SDL2.dll, an empty romfs\ and a stamp naming hookshot::kSourceCommit). Nothing is ever started.
# The shots: card and settings in both states; the Wii U game's settings (no "Plays with" for other games); the headless Play path
#   (not built, built with runner=auto, built with runner=lichii); the AV1 prompt at 1600x1000 (only that size).
param([string]$DataFrom = "C:\Users\fatty\Desktop\smashClassic\userdata", [string]$Build = "", [string]$Out = "",
      [string[]]$Sizes = @("1280x800", "1600x1000"), [int]$Secs = 12, [int]$Scroll = 30)
$ErrorActionPreference = "Stop"
$OOT = "switch:0400486024dec000"
$WIIU = "wiiu:0005000010145c00"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) {
    $Build = Join-Path $root "build\bin\Release"
    if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { $Build = Join-Path $root "..\hookshot\build\bin\Release" }
}
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { throw "no RetroHub.exe in $Build" }
$Build = (Resolve-Path $Build).Path
if (-not (Test-Path (Join-Path $DataFrom "library_cache.json"))) { throw "DataFrom has no library_cache.json: $DataFrom" }
if (-not $Out) { $Out = Join-Path $root "qa_out\hookshot-ui" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Sizes = @($Sizes | ForEach-Object { "$_" -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing

# ---- an isolated copy of the library (small top-level files + art), optionally with the fake Hookshot build and a vault settings file ----
function New-Copy([string]$tag, [bool]$ready, [string]$gameSettingsJson = "") {
    $run = Join-Path $env:TEMP ("rh_hsui_" + $tag + "_" + [guid]::NewGuid().ToString("N").Substring(0, 6))
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
    if ($ready) {
        $h = Join-Path $ud "emulators\hookshot"
        New-Item -ItemType Directory -Force (Join-Path $h "romfs") | Out-Null
        foreach ($f in "hookshot.exe", "hookshot_vk.dll", "SDL2.dll") { [IO.File]::WriteAllBytes((Join-Path $h $f), [byte[]]@()) }
        [IO.File]::WriteAllText((Join-Path $h "retrohub_hookshot.json"), '{"commit":"137c423589f178bb4274e339b17b4dfd466435e9","game_build":"e0f3ccfa","built":1794066770,"source":"x","size":1}')
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
    # HUB_SHOT_VAULT: headless runs ignore hub.json's vault_dir (main.cpp), so the copy's vault is named here
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
    if (Test-Path $tr) { Copy-Item $tr (Join-Path $Out "$name.trace.txt") -Force; Write-Host "  trace:"; Get-Content $tr | Where-Object { $_ -match '^(external|notice|hero)' } | ForEach-Object { Write-Host "    $_" } }
    $al = Join-Path $run "userdata\autotest.log"
    if (Test-Path $al) { Copy-Item $al (Join-Path $Out "$name.autotest.log") -Force; Write-Host "  autotest.log copied" }
}

# Shot: a card or Settings shot scrolls the details modal down (HUB_SHOT_WHEEL=x,y,ticks): the Hookshot card sits at the end of About and
# the "Plays with" row in the Gameplay group, both below the fold of the modal. Pass -Scroll 0 to shoot the top of the tab instead.
function Shot([string]$name, [string]$run, [string]$size, [hashtable]$extra) {
    $e = @{ HUB_SHOT_SIZE = $size }
    foreach ($k in $extra.Keys) { $e[$k] = $extra[$k] }
    if ($Scroll -gt 0 -and $size -match '^(\d+)x(\d+)$') {
        $e["HUB_SHOT_WHEEL"] = ("{0},{1},{2}" -f [int]([int]$Matches[1] * 0.5), [int]([int]$Matches[2] * 0.6), $Scroll)
    }
    Invoke-Run $run $e
    Save-Result $run "$name-$size"
}

$lichiiJson = '{"version": "1", "systems": {}, "games": {"' + $OOT + '": {"switch.runner": "lichii"}}}'
$copies = @()
try {
    $notbuilt = New-Copy "nb" $false;  $copies += $notbuilt
    $ready = New-Copy "rd" $true;      $copies += $ready
    $lichii = New-Copy "lc" $true $lichiiJson; $copies += $lichii
    Write-Host "copies: $notbuilt | $ready | $lichii"

    foreach ($s in $Sizes) {
        Shot "card-notbuilt" $notbuilt $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "0" }
        Shot "settings-notbuilt" $notbuilt $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "3" }
        Shot "card-ready" $ready $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "0" }
        Shot "settings-ready" $ready $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "3" }
        Shot "settings-ready-lichii" $lichii $s @{ HUB_SHOT_DETAILS = $OOT; HUB_SHOT_TAB = "3" }
        Shot "settings-wiiu-notbuilt" $notbuilt $s @{ HUB_SHOT_DETAILS = $WIIU; HUB_SHOT_TAB = "3" }
    }

    # headless Play: RETROHUB_AUTOTEST=play presses the Play button's code path (play_entry -> launch_external) for Ocarina.
    # Headless launches are skipped; the trace line says which path was chosen.
    $playEnv = @{ RETROHUB_AUTOTEST = "play"; RETROHUB_AUTOTEST_GAME = $OOT; HUB_SHOT_SECS = "25"; HUB_SHOT_SIZE = $Sizes[0] }
    foreach ($pair in @(@("play-notbuilt", $notbuilt), @("play-ready", $ready), @("play-ready-lichii", $lichii))) {
        Invoke-Run $pair[1] $playEnv
        Save-Result $pair[1] ($pair[0] + "-" + $Sizes[0])
    }

    # the AV1 prompt (no game needed), 1600x1000 only
    Shot "av1-prompt" $notbuilt "1600x1000" @{ HUB_SHOT_EMU = "hookshotav1" }
}
finally {
    foreach ($c in $copies) { Remove-Item -LiteralPath $c -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host "screenshots in $Out"
exit 0   # (robocopy's exit code would otherwise be the script's)
