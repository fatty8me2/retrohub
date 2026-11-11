# Standard screenshot matrix: every reachable page/state x 1280x720 and 1920x1080 (plus 1024x600 for the key pages), headless, muted, one hub at a time.
# Output folder: PNGs, contact sheets (qa\contact_sheet.ps1 -Full, 12 shots per sheet at 1280x720), index.txt (page | env vars | size | file | status) and summary.txt.
# Safety: refuses to start (exit 3) while the owner is gaming (a game/emulator process whose Path is NOT under ...\SmashClassicPort\wt\); re-checks before every shot and PAUSES (60 s polls) if one appears mid-run.
# The hub runs from a SANDBOX COPY of the build output (no userdata), with a fresh sandbox userdata + HUB_SHOT_VAULT before every shot: nothing real is read or written.
# Failures reported at the end (exit 1): non-zero hub exit, missing/empty file, all-black or single-colour image, timeout (the hub is killed by PID only).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\shot_matrix.ps1 [-Out <dir>] [-Build <bin\Release>] [-Only <name-regex>] [-Sizes 1280x720,1920x1080] [-NoKey]
param([string]$Out = "", [string]$Build = "", [string]$Only = "", [string[]]$Sizes = @("1280x720", "1920x1080"), [switch]$NoKey, [int]$TimeoutSec = 90)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot "_gamecheck.ps1")
if (Test-OwnerGaming) {
    Write-Host ("REFUSED: the owner is gaming (" + ((Get-OwnerGameProcs | ForEach-Object { $_.Name }) -join ",") + "). Re-run when no game is running.")
    exit 3
}
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
if (-not (Test-Path (Join-Path $Build "RetroHub.exe"))) { throw "no RetroHub.exe in $Build" }
$Build = (Resolve-Path $Build).Path
if (-not $Out) { $Out = Join-Path $root ("build\qa_matrix_out") }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
$Sizes = @($Sizes | ForEach-Object { "$_" -split "," } | Where-Object { $_ })
Add-Type -AssemblyName System.Drawing

# ---- sandbox copy of the build output (never a userdata folder) ----
$sand = Join-Path $root "build\qa_x"
New-Item -ItemType Directory -Force $sand | Out-Null
& robocopy $Build $sand /MIR /XD userdata /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
$exe = Join-Path $sand "RetroHub.exe"
if (-not (Test-Path $exe)) { throw "sandbox copy failed: $exe" }
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

# ---- the matrix: name, env, key (also at 1024x600) ----
$OOT = "switch:0400486024dec000"
$M = @(
    @{ n = "home";            k = 1; e = @{ HUB_SHOT_FAKE_LIB = "14" } },
    @{ n = "home-continue";   k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_CONTINUE = "1" } },
    @{ n = "home-empty";      k = 1; e = @{ HUB_SHOT_FAKE_LIB = "0" } },
    @{ n = "home-native-only"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "native" } },
    @{ n = "library-all";     k = 1; e = @{ HUB_SHOT_FAKE_LIB = "40"; HUB_START_VIEW = "all" } },
    @{ n = "library-list";    k = 0; e = @{ HUB_SHOT_FAKE_LIB = "40"; HUB_START_VIEW = "all"; HUB_SHOT_LIBVIEW = "list" } },
    @{ n = "details";         k = 1; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_DETAILS = "fake:2" } },
    @{ n = "details-playtime"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PLAYTIME = "1"; HUB_SHOT_DETAILS = "fake:0" } },
    @{ n = "details-saves-tab"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_DETAILS = "fake:2"; HUB_SHOT_TAB = "1" } },
    @{ n = "discover";        k = 1; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "discover" } },
    @{ n = "online-home";     k = 1; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "online" } },
    @{ n = "online-offline-home"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_OFFLINE = "home" } },
    @{ n = "online-offline-room"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_OFFLINE = "room" } },
    @{ n = "online-fake-room"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_FAKE_ROOM = "1" } },
    @{ n = "saves";           k = 1; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves" } },
    @{ n = "saves-cloud-checking"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "checking" } },
    @{ n = "saves-cloud-ok";  k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "ok" } },
    @{ n = "saves-cloud-mixed"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "mixed" } },
    @{ n = "saves-cloud-over"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "over" } },
    @{ n = "saves-cloud-attention"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "attention" } },
    @{ n = "saves-cloud-offline"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "offline" } },
    @{ n = "saves-cloud-paused"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "paused" } },
    @{ n = "saves-cloud-fresh"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "saves"; HUB_SHOT_CLOUD2 = "fresh" } },
    @{ n = "controllers";     k = 1; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_FAKE_PADS = "2" } },
    @{ n = "settings-general"; k = 1; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "settings:general" } },
    @{ n = "settings-online"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "settings:online" } },
    @{ n = "settings-video";  k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "settings:video" } },
    @{ n = "settings-account"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "settings:account" } },
    @{ n = "settings-help";   k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "settings:help" } },
    @{ n = "updates-about";   k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_PAGE = "about" } },
    @{ n = "notes-ready";     k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_UPDATE_NOTES = "ready" } },
    @{ n = "notes-failed-rollback"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_UPDATE_NOTES = "failed,postponed,rollback,interrupted" } },
    @{ n = "friends-panel";   k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FRIENDS = "1" } },
    @{ n = "friends-bell";    k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FRIENDS = "1"; HUB_SHOT_BELL = "1" } },
    @{ n = "hover-tooltip";   k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_FAKE_FRIENDS = "1"; HUB_SHOT_FRIENDS = "1"; HUB_SHOT_HOVER = "@BELL@" } },
    @{ n = "hookshot-idle";   k = 0; e = @{ HUB_SHOT_FAKE_LIB = "1"; HUB_SHOT_HOOKSHOT = "idle" } },
    @{ n = "hookshot-problems"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "1"; HUB_SHOT_HOOKSHOT = "problems" } },
    @{ n = "hookshot-building"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "1"; HUB_SHOT_HOOKSHOT = "building" } },
    @{ n = "hookshot-done";   k = 0; e = @{ HUB_SHOT_FAKE_LIB = "1"; HUB_SHOT_HOOKSHOT = "done" } },
    @{ n = "hookshot-fritchii"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "1"; HUB_SHOT_HOOKSHOT = "done_fritchii" } },
    @{ n = "emu-ask-azahar";  k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_EMU = "azahar" } },
    @{ n = "emu-working-azahar-42"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_EMU = "working:azahar:42" } },
    @{ n = "emu-working-cemu-90"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_EMU = "working:cemu:90" } },
    @{ n = "emu-failed";      k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_EMU = "failed" } },
    @{ n = "emu-lichii-update"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_EMU = "lichiiupdate" } },
    @{ n = "welcome-whats-new"; k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_WELCOME = "1" } },
    @{ n = "lock-pending";    k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_ACCESS = "pending" } },
    @{ n = "admin";           k = 0; e = @{ HUB_SHOT_FAKE_LIB = "14"; HUB_SHOT_ADMIN = "1" } }
)
if ($Only) { $M = @($M | Where-Object { $_.n -match $Only }) }

$base = @{ RETROHUB_NO_SHELL = "1"; RETROHUB_QA = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SHOT_SECS = "3"; HUB_SHOT_NOWAIT = "1" }
$userdata = Join-Path $sand "userdata"
$vault = Join-Path $sand "vault"
$rows = New-Object System.Collections.ArrayList
$fails = New-Object System.Collections.ArrayList
$aborted = $false

function Test-Blank([string]$png) {   # all-black / single-colour image: a grid of samples with (almost) no distinct colours
    $b = New-Object System.Drawing.Bitmap $png
    try {
        $seen = @{}
        for ($y = 3; $y -lt $b.Height; $y += [math]::Max(1, [int]($b.Height / 24))) {
            for ($x = 3; $x -lt $b.Width; $x += [math]::Max(1, [int]($b.Width / 40))) {
                $c = $b.GetPixel($x, $y)
                $seen["$([int]($c.R/8)),$([int]($c.G/8)),$([int]($c.B/8))"] = 1
            }
        }
        return ($seen.Count -lt 3)
    } finally { $b.Dispose() }
}

function Run-Shot($m, [string]$size) {
    if (Test-OwnerGaming) { Write-Host "a game started mid-run: pausing until it is closed"; Wait-NoGame }   # never run a hub next to a game; resume (re-checked every 60 s) once it is gone
    foreach ($d in $userdata, $vault) { if (Test-Path $d) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue } }   # fresh sandbox state per shot
    New-Item -ItemType Directory -Force (Join-Path $vault "_account") | Out-Null
    $name = "$($m.n)-$size"
    $bmp = Join-Path $Out "$name.bmp"
    $png = Join-Path $Out "$name.png"
    $errf = Join-Path $Out "$name.stderr.txt"
    $env2 = @{}
    foreach ($k in $base.Keys) { $env2[$k] = $base[$k] }
    foreach ($k in $m.e.Keys) { $env2[$k] = $m.e[$k] }
    if ($env2["HUB_SHOT_HOVER"] -eq "@BELL@") {   # the bell button of the friends header: right edge, near the top
        $w = [int]($size.Split("x")[0]); $env2["HUB_SHOT_HOVER"] = "$($w - 100),$([int]([int]($size.Split('x')[1]) * 0.045))"
    }
    $env2["HUB_SCREENSHOT"] = $bmp; $env2["HUB_SHOT_SIZE"] = $size; $env2["HUB_SHOT_VAULT"] = $vault
    $psi = New-Object System.Diagnostics.ProcessStartInfo $exe
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WorkingDirectory = $sand
    $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
    foreach ($k in $env2.Keys) { $psi.EnvironmentVariables[$k] = [string]$env2[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync(); $outTask = $p.StandardOutput.ReadToEndAsync()
    $timedOut = $false
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $timedOut = $true; try { Stop-Process -Id $p.Id -Force } catch {} }   # our own PID only
    $p.WaitForExit()
    $code = $p.ExitCode
    [IO.File]::WriteAllText($errf, $errTask.Result)
    $envText = (($m.e.Keys | Sort-Object | ForEach-Object { "$_=$($m.e[$_])" }) -join " ")
    $status = "ok"
    if ($timedOut) { $status = "FAIL timeout" }
    elseif ($code -ne 0) { $status = "FAIL exit=$code" }
    elseif (-not (Test-Path $bmp) -or (Get-Item $bmp).Length -lt 1000) { $status = "FAIL missing-file" }
    else {
        $img = [System.Drawing.Image]::FromFile($bmp); $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
        Remove-Item $bmp -ErrorAction SilentlyContinue
        if (Test-Blank $png) { $status = "FAIL blank-image" }
    }
    if (Test-Path $bmp) { Remove-Item $bmp -ErrorAction SilentlyContinue }
    if ($status -eq "ok") { Remove-Item $errf -ErrorAction SilentlyContinue }
    [void]$rows.Add([pscustomobject]@{ Page = $m.n; Env = $envText; Size = $size; File = "$name.png"; Status = $status; Png = $png })
    if ($status -ne "ok") { [void]$fails.Add("$name : $status") }
    Write-Host ("{0,-34} {1,-10} {2}" -f $m.n, $size, $status)
    return $status
}

$t0 = Get-Date
foreach ($m in $M) {
    $sz = @($Sizes)
    if ($m.k -and -not $NoKey -and ($sz -notcontains "1024x600")) { $sz += "1024x600" }
    foreach ($s in $sz) {
        if ((Run-Shot $m $s) -eq "ABORT") { $aborted = $true; break }
    }
    if ($aborted) { break }
}

# ---- index, contact sheets, summary ----
$idx = New-Object System.Collections.ArrayList
[void]$idx.Add("page | env vars | size | file | status")
foreach ($r in $rows) { [void]$idx.Add("$($r.Page) | $($r.Env) | $($r.Size) | $($r.File) | $($r.Status)") }
[IO.File]::WriteAllLines((Join-Path $Out "index.txt"), $idx)
foreach ($s in $Sizes) {
    $files = @($rows | Where-Object { $_.Size -eq $s -and $_.Status -eq "ok" } | ForEach-Object { $_.Png })
    for ($i = 0; $i * 12 -lt $files.Count; $i++) {
        $chunk = @($files | Select-Object -Skip ($i * 12) -First 12)
        $sheet = Join-Path $Out ("contact-$s-" + ($i + 1) + ".png")
        try { & (Join-Path $PSScriptRoot "contact_sheet.ps1") $sheet @chunk -Cols 3 -Full } catch { [void]$fails.Add("contact sheet $sheet : $($_.Exception.Message)") }
    }
}
$sum = New-Object System.Collections.ArrayList
[void]$sum.Add("shots: $($rows.Count)   failures: $($fails.Count)   elapsed: $([int]((Get-Date) - $t0).TotalSeconds) s" + $(if ($aborted) { "   ABORTED: the owner started a game" } else { "" }))
foreach ($f in $fails) { [void]$sum.Add("FAIL  $f") }
[IO.File]::WriteAllLines((Join-Path $Out "summary.txt"), $sum)
$sum | ForEach-Object { Write-Host $_ }
Write-Host "output: $Out"
if ($aborted) { exit 3 }
if ($fails.Count -gt 0) { exit 1 }
exit 0
