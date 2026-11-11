# Headless shots of the "<Emulator> stopped unexpectedly" card (HUB_SHOT_CRASHNOTE=dump|nodump|start, honoured ONLY in screenshot mode; the fake touches no files and starts nothing).
# Runs a sandbox copy of the build (no userdata folder), hidden, muted, killed by PID if it overruns. Waits while a game or emulator is running.
# Usage: qa\crashnote_shots.ps1 [-Build <bin\Release>] [-Out <dir>] [-Sizes 1280x720,1024x600] [-Secs 8]
param([string]$Build = "", [string]$Out = "", [string[]]$Sizes = @("1280x720", "1024x600"), [int]$Secs = 8)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { throw "no RetroHub.exe in $Build" }
$Build = (Resolve-Path $Build).Path
if (-not $Out) { $Out = Join-Path $root "qa_out\crashnote" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$Sizes = @($Sizes | ForEach-Object { "$_" -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing

function Wait-Quiet {
    while (Get-Process lichii, eden-cli, hookshot, Dolphin, Cemu, azahar, pcsx2-qt, PPSSPPWindows64, retrorun, n64run -ErrorAction SilentlyContinue) { Write-Host "a game or emulator is running; waiting 60 s"; Start-Sleep -Seconds 60 }
}

$run = Join-Path $env:TEMP ("rh_crashnote_" + [guid]::NewGuid().ToString("N").Substring(0, 6))
New-Item -ItemType Directory -Force $run | Out-Null
try {
    Copy-Item (Join-Path $Build "RetroHub.exe"), (Join-Path $Build "SDL2.dll") $run
    foreach ($d in "data", "controls") { if (Test-Path (Join-Path $Build $d)) { Copy-Item (Join-Path $Build $d) (Join-Path $run $d) -Recurse } }
    New-Item -ItemType Directory -Force (Join-Path $run "vault") | Out-Null
    foreach ($size in $Sizes) {
        foreach ($mode in "dump", "nodump", "start") {
            Wait-Quiet
            $bmp = Join-Path $run "shot.bmp"
            Remove-Item -LiteralPath $bmp -ErrorAction SilentlyContinue
            $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SECS = "$Secs";
                      HUB_SHOT_VAULT = (Join-Path $run "vault"); HUB_SHOT_SIZE = $size; HUB_SHOT_CRASHNOTE = $mode }
            try {
                foreach ($k in $set.Keys) { Set-Item -Path "env:$k" -Value $set[$k] }
                $p = Start-Process (Join-Path $run "RetroHub.exe") -WorkingDirectory $run -PassThru -WindowStyle Hidden
                if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; Write-Host "overran: killed by PID $($p.Id)" }
            } finally { foreach ($k in $set.Keys) { Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue } }
            if (Test-Path $bmp) {
                $img = [System.Drawing.Image]::FromFile($bmp)
                $png = Join-Path $Out "crashnote-$mode-$size.png"
                $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
                $img.Dispose()
                Write-Host "shot $png"
            } else { Write-Host "NO SHOT: $mode $size" }
        }
    }
} finally { Remove-Item -LiteralPath $run -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "screenshots in $Out"
exit 0
