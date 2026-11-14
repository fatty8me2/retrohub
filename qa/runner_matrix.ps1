# Play-path matrix for the Switch "Plays with" choice (switch.runner) of Ocarina of Time (Switch 2), headless.
# Each row gets its own scratch copy of RetroHub (temp folder, deleted at the end) with fake emulator installs, fake native builds
# (Hookshot / Longshot) and a per-game switch.runner in a scratch vault. RETROHUB_AUTOTEST=play presses the Play button's code path
# (play_entry -> launch_external) and the headless branch of launch_external writes ONE trace line (HUB_SHOT_TRACE) that says what
# Play would start: "external <id> ready | <exe>", "external <id> not built | <why>", "external <emu> installed | ..." or
# "external <emu> not-installed -> install dialog". Nothing is ever spawned: the headless branch returns before any process starts.
# Hidden window, muted, no network (hub.json points at 127.0.0.1:9 and HUB_SHOT_NET is never set), killed by PID on overrun.
# The real install (C:\Users\fatty\Desktop\smashClassic) and %LOCALAPPDATA%\RetroHub are only LISTED before and after (top level), never read or written.
# Usage: qa\runner_matrix.ps1 [-Build <bin\Release>] [-Out <dir>] [-Secs 25] [-TimeoutSec 150] [-Shots] [-KeepWork]
#   -Shots  also captures the two "not built" dialogs (HUB_SHOT_EMU=hookshotmissing / longshotmissing) as PNGs in -Out.
param([string]$Build = "", [string]$Out = "", [int]$Secs = 25, [int]$TimeoutSec = 150, [switch]$Shots, [switch]$KeepWork)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\_gamecheck.ps1"
Add-Type -AssemblyName System.Drawing

$OOT = "switch:0400486024dec000"
$HS_COMMIT = "8ef84611049f4fc31835a0b12793cf074ed49c3c"   # hookshot.cpp kInfo[Hookshot].source_commit (= kSourceCommit)
$LS_COMMIT = "5c1f00dd444e735bf27b73b2f91c0617f930ddac"   # hookshot.cpp kInfo[Longshot].source_commit
$LICHII_SHA = "e6ac8c09ddd00cb64e539f2a85f9f9f32d579c772b3619aed0d4d3910a93e833"   # the same pins as qa\fritchii_shots.ps1
$FRIT_SHA = "068fb4c66b2e32447ac7690393039043854de4b189aa7e95b34c4bf0d2a4080e"
$utf8 = New-Object System.Text.UTF8Encoding $false

$root = Split-Path $PSScriptRoot -Parent
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { Write-Host "STOP: no RetroHub.exe in $Build (not rebuilding)"; exit 3 }
$Build = (Resolve-Path $Build).Path
if (-not $Out) { $Out = Join-Path $root "qa_out\runner-matrix" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
if (Test-OwnerGaming) { Write-Host ("owner is gaming (" + ((Get-OwnerGameProcs | ForEach-Object { $_.Name }) -join ",") + "): not starting"); exit 4 }

function Snap([string]$dir) {   # top level only: names, sizes, write times
    if (-not (Test-Path -LiteralPath $dir)) { return "(absent)" }
    return ((Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + "|" + $_.Length + "|" + $_.LastWriteTimeUtc.Ticks }) | Sort-Object) -join "`n"
}
$guardPaths = @("C:\Users\fatty\Desktop\smashClassic\userdata", (Join-Path $env:LOCALAPPDATA "RetroHub"))
$snapBefore = $guardPaths | ForEach-Object { Snap $_ }

$Work = Join-Path $env:TEMP ("rh_runner_matrix_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $Work | Out-Null

function Write-Text([string]$path, [string]$text) {
    New-Item -ItemType Directory -Force (Split-Path $path -Parent) | Out-Null
    [IO.File]::WriteAllText($path, $text, $utf8)
}

# fake emulator install (same shape as qa\fritchii_shots.ps1: an MZ stub plus the install stamp that the hub's pin check reads)
function New-Emu([string]$ud, [string]$id) {
    $dir = Join-Path $ud "emulators\$id"
    New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $dir "lichii.exe"), [byte[]]@(0x4D, 0x5A))
    if ($id -eq "lichii") { Write-Text (Join-Path $dir "retrohub-install.txt") "Lichii build 177`nsha256 $LICHII_SHA`n" }
    else { Write-Text (Join-Path $dir "retrohub-install.txt") "Fritchii v4.1`nsha256 $FRIT_SHA`n" }
}

# fake native build: the files ready() checks (hookshot.exe, hookshot_vk.dll, SDL2.dll, romfs\) and the stamp with its source commit
function New-Native([string]$ud, [string]$flavor, [string]$commit) {
    $dir = Join-Path $ud "emulators\$flavor"
    New-Item -ItemType Directory -Force (Join-Path $dir "romfs") | Out-Null
    foreach ($f in "hookshot.exe", "hookshot_vk.dll", "SDL2.dll") { [IO.File]::WriteAllBytes((Join-Path $dir $f), [byte[]]@()) }
    Write-Text (Join-Path $dir "retrohub_hookshot.json") ('{"commit":"' + $commit + '","game_build":"e0f3ccfa","built":1794066770,"source":"qa-fake","size":1}')
}

# one scratch RetroHub: exe + SDL2 + data\ + controls\, a userdata with hub.json, the emulators and natives, and the vault's game_settings.json
function New-Run([string]$id, [string]$emus, [object[]]$natives, [string]$runner) {
    $run = Join-Path $Work ("row" + $id)
    New-Item -ItemType Directory -Force $run | Out-Null
    Copy-Item (Join-Path $Build "RetroHub.exe"), (Join-Path $Build "SDL2.dll") $run
    foreach ($d in "data", "controls") { if (Test-Path (Join-Path $Build $d)) { Copy-Item (Join-Path $Build $d) (Join-Path $run $d) -Recurse } }
    $ud = Join-Path $run "userdata"
    New-Item -ItemType Directory -Force $ud | Out-Null
    Write-Text (Join-Path $ud "hub.json") '{"name":"qa-Runner","rom_folders":[],"server":"ws://127.0.0.1:9/","sync_auto":false,"cloud_saves":false}'
    foreach ($e in @($emus -split ",")) { if ($e) { New-Emu $ud $e } }
    foreach ($n in $natives) { New-Native $ud $n.flavor $n.commit }
    if ($runner) { Write-Text (Join-Path $ud "vault\_settings\game_settings.json") ('{"version": "1", "systems": {}, "games": {"' + $OOT + '": {"switch.runner": "' + $runner + '"}}}') }
    return $run
}

# one hidden, muted, headless RetroHub run. The library is FAKE (HUB_SHOT_FAKE_LIB=1 + HUB_SHOT_SWITCH=2 seeds Ocarina of Time).
function Invoke-Run([string]$run, [hashtable]$extra, [int]$timeoutSec) {
    $set = @{ RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; RETROHUB_QA = "1";
              HUB_SCREENSHOT = (Join-Path $run "shot.bmp"); HUB_SHOT_TRACE = (Join-Path $run "shot_trace.txt"); HUB_SHOT_SECS = "$Secs";
              HUB_SHOT_VAULT = (Join-Path $run "userdata\vault"); HUB_SHOT_SIZE = "960x600"; HUB_SHOT_FAKE_LIB = "1"; HUB_SHOT_SWITCH = "2";
              RETROHUB_ALLOW_VERSION_MISMATCH = "1"; RETROHUB_NO_UPDATE_CHECK = "1" }
    foreach ($k in $extra.Keys) { $set[$k] = [string]$extra[$k] }
    try {
        foreach ($k in $set.Keys) { Set-Item -Path "env:$k" -Value $set[$k] }
        $p = Start-Process (Join-Path $run "RetroHub.exe") -WorkingDirectory $run -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit($timeoutSec * 1000)) { Stop-Process -Id $p.Id -Force; Write-Host "  overran: killed by PID $($p.Id)" }
    } finally {
        foreach ($k in $set.Keys) { Remove-Item -Path "env:$k" -ErrorAction SilentlyContinue }
    }
}

$rows = @(
    @{ id = "01"; label = "unset / nothing";                         emus = "lichii,fritchii"; natives = @();                                              runner = $null;      expect = '(?i)^external fritchii\b.*\binstalled\b' },
    @{ id = "02"; label = "lichii / nothing";                        emus = "lichii,fritchii"; natives = @();                                              runner = "lichii";   expect = '(?i)^external lichii\b.*\binstalled\b' },
    @{ id = "03"; label = "fritchii / nothing";                      emus = "lichii,fritchii"; natives = @();                                              runner = "fritchii"; expect = '(?i)^external fritchii\b.*\binstalled\b' },
    @{ id = "04"; label = "hookshot / nothing";                      emus = "lichii,fritchii"; natives = @();                                              runner = "hookshot"; expect = '^external hookshot not built \| ' },
    @{ id = "05"; label = "longshot / nothing";                      emus = "lichii,fritchii"; natives = @();                                              runner = "longshot"; expect = '^external longshot not built \| ' },
    @{ id = "06"; label = "hookshot / hookshot built";               emus = "lichii,fritchii"; natives = @(@{ flavor = "hookshot"; commit = $HS_COMMIT }); runner = "hookshot"; expect = '^external hookshot ready \| .*emulators[\\/]hookshot[\\/]hookshot\.exe$' },
    @{ id = "07"; label = "longshot / longshot built";               emus = "lichii,fritchii"; natives = @(@{ flavor = "longshot"; commit = $LS_COMMIT }); runner = "longshot"; expect = '^external longshot ready \| .*emulators[\\/]longshot[\\/]hookshot\.exe$' },
    @{ id = "08"; label = "hookshot / only longshot built";           emus = "lichii,fritchii"; natives = @(@{ flavor = "longshot"; commit = $LS_COMMIT }); runner = "hookshot"; expect = '^external hookshot not built \| ' },
    @{ id = "09"; label = "longshot / only hookshot built";           emus = "lichii,fritchii"; natives = @(@{ flavor = "hookshot"; commit = $HS_COMMIT }); runner = "longshot"; expect = '^external longshot not built \| ' },
    @{ id = "10"; label = "longshot / both built";                   emus = "lichii,fritchii"; natives = @(@{ flavor = "hookshot"; commit = $HS_COMMIT }, @{ flavor = "longshot"; commit = $LS_COMMIT }); runner = "longshot"; expect = '^external longshot ready \| .*emulators[\\/]longshot[\\/]hookshot\.exe$' },
    @{ id = "11"; label = "lichii / both built";                     emus = "lichii,fritchii"; natives = @(@{ flavor = "hookshot"; commit = $HS_COMMIT }, @{ flavor = "longshot"; commit = $LS_COMMIT }); runner = "lichii"; expect = '(?i)^external lichii\b.*\binstalled\b' },
    @{ id = "12"; label = "hookshot / hookshot folder with Longshot commit"; emus = "lichii,fritchii"; natives = @(@{ flavor = "hookshot"; commit = $LS_COMMIT }); runner = "hookshot"; expect = '^external hookshot not built \| .*another' },
    @{ id = "1b"; label = "(extra) unset / Lichii installed, Fritchii not"; emus = "lichii"; natives = @();                                              runner = $null;      expect = '(?i)^external fritchii not-installed' }
)

$results = @()
foreach ($r in $rows) {
    $run = New-Run $r.id $r.emus $r.natives $r.runner
    Invoke-Run $run @{ RETROHUB_AUTOTEST = "play"; RETROHUB_AUTOTEST_GAME = $OOT; RETROHUB_AUTOTEST_NAME = "qa-Runner" } $TimeoutSec
    $trFile = Join-Path $run "shot_trace.txt"
    $alFile = Join-Path $run "userdata\autotest.log"
    $tr = if (Test-Path $trFile) { @(Get-Content $trFile | Where-Object { $_ -match '^external ' }) } else { @() }
    $al = if (Test-Path $alFile) { @(Get-Content $alFile) } else { @() }
    if (Test-Path $trFile) { Copy-Item $trFile (Join-Path $Out ("row" + $r.id + ".trace.txt")) -Force }
    if (Test-Path $alFile) { Copy-Item $alFile (Join-Path $Out ("row" + $r.id + ".autotest.log")) -Force }
    $playLines = @($al | Where-Object { $_ -match '^(play|error)' })
    $spawned = @($al | Where-Object { $_ -match 'external: (starting|spawn)' })
    $hit = @($tr | Where-Object { $_ -match $r.expect })
    $ok = ($tr.Count -eq 1) -and ($hit.Count -eq 1) -and ($spawned.Count -eq 0)
    $actual = if ($tr.Count -eq 0) { "(no external trace; autotest: " + ($playLines -join " / ") + ")" } else { $tr -join " || " }
    if ($spawned.Count -gt 0) { $actual += "  [SPAWN: " + ($spawned -join " / ") + "]" }
    $results += [pscustomobject]@{ Row = $r.id; Setup = $r.label; Expected = $r.expect; Actual = $actual; Result = $(if ($ok) { "PASS" } else { "FAIL" }) }
    Write-Host ("row " + $r.id + " " + $r.label + " -> " + $(if ($ok) { "PASS" } else { "FAIL" }) + " | " + $actual)
}

if ($Shots) {
    $run = New-Run "shots" "lichii,fritchii" @() $null
    foreach ($m in "hookshotmissing", "longshotmissing") {
        Invoke-Run $run @{ HUB_SHOT_EMU = $m; HUB_SHOT_SIZE = "1280x800" } $TimeoutSec
        $png = Join-Path $Out ("dialog-" + $m + "-1280x800.png")
        $bmp = Join-Path $run "shot.bmp"
        if (Test-Path $bmp) { $img = [System.Drawing.Image]::FromFile($bmp); $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose(); Write-Host "shot $png" }
        else { Write-Host "NO SHOT: $m (exit without a screenshot)" }
    }
}

$snapAfter = $guardPaths | ForEach-Object { Snap $_ }
$untouched = (($snapBefore -join "`n") -eq ($snapAfter -join "`n"))
Write-Host ""
$results | Format-Table Row, Result, Setup -AutoSize | Out-String -Width 200 | Write-Host
$results | ForEach-Object { "row " + $_.Row + " | " + $_.Result + " | " + $_.Setup + "`n    expected: " + $_.Expected + "`n    actual:   " + $_.Actual } | Set-Content -Path (Join-Path $Out "matrix.txt") -Encoding utf8
Write-Host ("real install / %LOCALAPPDATA%\RetroHub top-level listing unchanged: " + $untouched)
$fails = @($results | Where-Object { $_.Result -ne "PASS" }).Count
Write-Host ("RESULT: " + $(if ($fails) { "FAIL ($fails of $($results.Count))" } else { "PASS ($($results.Count) rows)" }) + "  out: $Out")
if (-not $KeepWork) { Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue }
else { Write-Host "work kept: $Work" }
exit $fails
