# Headless screenshots of the REAL cloud saves UI: RetroHub's real cloud engine against a LOCAL real Worker (wrangler dev, isolated
# registry, ports 10450-10479, loopback only). Replaces the obsolete fake-engine qa\cloud_ui_shots.ps1. Nothing visible, muted,
# never deploys, never touches the user's install. Kills only the PIDs it started (taskkill /PID /T /F).
#   powershell -ExecutionPolicy Bypass -File qa\cloud_ui_shots_real.ps1 [-Out qa\shots_real] [-Port 10455] [-ServerDir <...\server\cloudflare>]
param([string]$Out = "", [int]$Port = 10455, [string]$ServerDir = "", [string]$BuildDir = "")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $PSScriptRoot "shots_real" }
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build\bin\Release" }
if (-not $ServerDir) { $ServerDir = Join-Path (Split-Path -Parent $Root) "cloud-int-bs\server\cloudflare" }
if ($Port -lt 10450 -or $Port -gt 10478) { throw "Port must be 10450-10478" }
$Tool = Join-Path $BuildDir "qa_cloud_tool.exe"
$Exe = Join-Path $BuildDir "RetroHub.exe"
$Scratch = Join-Path $env:TEMP "rhq_cloud_shots"
if (Test-Path $Scratch) { Remove-Item $Scratch -Recurse -Force }
New-Item -ItemType Directory -Force $Scratch, $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:SSB64_MUTE = "1"
$env:QA_SERVER_DIR = $ServerDir
$script:workers = @(); $script:holds = @()
$Ver = (Get-Content (Join-Path $Root "VERSION") -Raw).Trim()

function Kill-Tree([int]$id) { cmd /c "taskkill /PID $id /T /F >nul 2>&1" | Out-Null }
function Start-Worker([int]$port, [string[]]$extra = @()) {
    $json = Join-Path $Scratch "worker$port.json"; $stop = Join-Path $Scratch "worker$port.stop"
    $a = @((Join-Path $PSScriptRoot "qa_worker_shots.mjs"), "--port", $port, "--out", $json, "--stop-file", $stop, "--parent-pid", $PID, "--accounts", 8) + $extra
    $p = Start-Process node -ArgumentList $a -WorkingDirectory $ServerDir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $Scratch "worker$port.out") -RedirectStandardError (Join-Path $Scratch "worker$port.err")
    $script:workers += @{ Pid = $p.Id; Stop = $stop }
    for ($i = 0; $i -lt 300 -and -not (Test-Path $json); $i++) { if ($p.HasExited) { break }; Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $json)) { throw "worker did not start (see $Scratch\worker$port.err)" }
    $j = Get-Content $json -Raw | ConvertFrom-Json
    @{ Base = $j.base; Accts = $j.accounts; Pid = $p.Id; Stop = $stop }
}
function Stop-Worker($w) { New-Item -ItemType File -Force $w.Stop | Out-Null; Start-Sleep -Seconds 3; Kill-Tree $w.Pid }

# one fresh "PC" = a copy of the hub exe with its own userdata (data dir) and vault (userdata\saves)
function New-Hub([string]$name, $w, [int]$acct, [hashtable]$settings) {
    $d = Join-Path $Scratch $name
    New-Item -ItemType Directory -Force $d | Out-Null
    foreach ($f in @("RetroHub.exe", "SDL2.dll")) { Copy-Item (Join-Path $BuildDir $f) $d }
    foreach ($dir in @("data", "controls")) { if (Test-Path (Join-Path $BuildDir $dir)) { Copy-Item (Join-Path $BuildDir $dir) (Join-Path $d $dir) -Recurse } }
    $ud = Join-Path $d "userdata"; New-Item -ItemType Directory -Force (Join-Path $ud "saves") | Out-Null
    $s = [ordered]@{ server = ($w.Base -replace "^http", "ws"); name = "QA"; username_claimed = $false; cloud_enabled = $true; setup_done = $true
                     find_games_done = $true; changelog_seen = $Ver; sync_card_dismissed = $true }
    foreach ($k in $settings.Keys) { $s[$k] = $settings[$k] }
    [IO.File]::WriteAllText((Join-Path $ud "hub.json"), ($s | ConvertTo-Json))
    $idj = '{"user_id":"' + $w.Accts[$acct].uid + '","secret":"' + $w.Accts[$acct].secret + '","created":1}'
    New-Item -ItemType Directory -Force (Join-Path $ud "saves\_identity") | Out-Null
    [IO.File]::WriteAllText((Join-Path $ud "identity.json"), $idj); [IO.File]::WriteAllText((Join-Path $ud "saves\_identity\identity.json"), $idj)
    @{ Dir = $d; Data = $ud; Vault = (Join-Path $ud "saves"); W = $w; Uid = $w.Accts[$acct].uid; Secret = $w.Accts[$acct].secret }
}
function Tool($pc, [string[]]$cmd, [string]$url = "", [string]$name = "this-pc") {
    if (-not $url) { $url = $pc.W.Base }
    if ($pc.Name) { $name = $pc.Name }
    $a = @("--data", $pc.Data, "--vault", $pc.Vault, "--url", $url, "--uid", $pc.Uid, "--secret", $pc.Secret, "--name", $name) + $cmd
    (& $Tool @a 2>&1 | Out-String).Trim().Replace("`n", " | ")
}
function Sess($pc, [string]$key, [string[]]$extra = @(), [string]$sys = "n64", [string]$url = "") { Tool $pc (@("session", $sys, $key) + $extra) $url }
function Put($pc, [string]$sys, [string]$key, [string]$rel, [string]$text) {
    $p = Join-Path $pc.Vault "$sys\$key\$($rel.Replace('/', '\'))"; New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null; [IO.File]::WriteAllText($p, $text)
}
# a second PC of the same account (own data dir + vault) sharing the worker
function Other-Pc($pc, [string]$name) {
    $d = Join-Path $Scratch "other_$name"; New-Item -ItemType Directory -Force (Join-Path $d "data"), (Join-Path $d "vault") | Out-Null
    @{ Name = "Laptop-$name"; Data = (Join-Path $d "data"); Vault = (Join-Path $d "vault"); W = $pc.W; Uid = $pc.Uid; Secret = $pc.Secret }
}

function Shot($pc, [string]$name, [hashtable]$vars, [double]$secs = 12, [string[]]$sizes = @("1280x800", "1280x1900")) {
    # every shot starts from the same pristine PC state (a second run on the same dir would be a "restart", not a first sync)
    $pri = Join-Path $pc.Dir "userdata.pristine"
    if (-not (Test-Path $pri)) { Copy-Item $pc.Data $pri -Recurse } 
    foreach ($size in $sizes) {
        if (Test-Path $pri) { Remove-Item $pc.Data -Recurse -Force; Copy-Item $pri $pc.Data -Recurse }
        $bmp = Join-Path $Out "${name}_$size.bmp"
        $set = @{ HUB_SCREENSHOT = $bmp; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "$secs"; HUB_SHOT_CLOUD_REAL = "1"; HUB_SHOT_SIZE = $size } + $vars
        foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
        $p = Start-Process -FilePath (Join-Path $pc.Dir "RetroHub.exe") -WorkingDirectory $pc.Dir -PassThru -WindowStyle Hidden
        if (-not $p.WaitForExit(90000)) { Kill-Tree $p.Id }
        foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
        if (Test-Path $bmp) {
            $im = [System.Drawing.Image]::FromFile($bmp); $im.Save((Join-Path $Out "${name}_$size.png"), [System.Drawing.Imaging.ImageFormat]::Png); $im.Dispose(); Remove-Item $bmp
        }
        Write-Output "${name}_$size -> $(Test-Path (Join-Path $Out "${name}_$size.png"))"
    }
}
$saves = @{ HUB_SHOT_PAGE = "saves" }
$big = "progress-" + ("A" * 3000)

try {
    $w = Start-Worker $Port
    Write-Host "worker $($w.Base) pid $($w.Pid)"

    # The hub is a FRESH second PC of the account (a device name that did not push the data), so its real engine restores/pulls on start.
    # (1) all synced
    $h = New-Hub "ok" $w 0 @{}
    $o = Other-Pc $h "ok"
    Sess $o "smash-64" @("--write", "battery/ssb64.sra", $big) | Out-Null
    Sess $o "zelda-ocarina" @("--write", "battery/zelda.sra", $big) | Out-Null
    Sess $o "pokemon-emerald" @("--write", "battery/emerald.sav", $big) "gba" | Out-Null
    Sess $o "kirby-nightmare" @("--write", "battery/kirby.sav", $big) "gba" | Out-Null
    Shot $h "saves_ok" $saves
    # same, but the hub restarts on the PC that pushed the data (known engine gap: games stay "Waiting" after a restart)
    $h = New-Hub "ok_restart" $w 6 @{}
    Sess $h "smash-64" @("--write", "battery/ssb64.sra", $big) | Out-Null
    Sess $h "pokemon-emerald" @("--write", "battery/emerald.sav", $big) "gba" | Out-Null
    Shot $h "saves_ok_restart_samepc" $saves 12 @("1280x1900")

    # (2) mixed: synced + pending conflict (local differs from another PC's cloud copy) + waiting/syncing (local-only new game, early shot)
    $h = New-Hub "mixed" $w 1 @{}
    # this-pc (the tool with the hub's own data dir) synced mario-kart and zelda before; another PC then pushed newer versions
    Sess $h "mario-kart-64" @("--write", "battery/mk.sra", "v1-" + ("M" * 2000)) | Out-Null
    Sess $h "zelda-ocarina" @("--write", "battery/zelda.sra", "v1-" + ("Z" * 2000)) | Out-Null
    $o = Other-Pc $h "mx"
    Sess $o "mario-kart-64" @("--write", "battery/mk.sra", "cloud-v2-" + ("C" * 2000)) | Out-Null
    Sess $o "zelda-ocarina" @("--write", "battery/zelda.sra", "cloud-v2-" + ("Z" * 2000)) | Out-Null
    Sess $o "smash-64" @("--write", "battery/ssb64.sra", $big) | Out-Null
    Put $h "n64" "mario-kart-64" "battery/mk.sra" ("local-v2-" + ("L" * 2000))   # edited here too: both sides changed -> conflict
    Write-Host (Sess $h "mario-kart-64" @("--resolve", "later"))                  # the player chose "decide later": a pending conflict
    Put $h "gba" "pokemon-emerald" "battery/emerald.sav" $big                   # brand new game here: waiting / backing up
    Shot $h "saves_mixed" $saves 20
    Shot $h "saves_mixed_early" $saves 1.2 @("1280x800")
    Shot $h "saves_history" ($saves + @{ HUB_SHOT_CLOUD_OPEN = "n64/zelda-ocarina" }) 20 @("1280x1900")

    # (3) offline: data synced on a live server, then the hub starts against a port with nothing listening
    $h = New-Hub "offline" $w 2 @{ server = "ws://127.0.0.1:1" }
    $h2 = New-Hub "offline_seed" $w 2 @{}
    Sess $h2 "smash-64" @("--write", "battery/ssb64.sra", $big) | Out-Null
    Sess $h2 "pokemon-emerald" @("--write", "battery/emerald.sav", $big) "gba" | Out-Null
    Copy-Item (Join-Path $h2.Vault "*") $h.Vault -Recurse -Force
    Copy-Item (Join-Path $h2.Data "*") $h.Data -Recurse -Force -Exclude "hub.json"
    Shot $h "saves_offline" $saves 8

    # (5) playing on another PC
    $h = New-Hub "lease" $w 3 @{}
    Sess $h "zelda-ocarina" @("--write", "battery/zelda.sra", $big) | Out-Null     # this PC already has zelda in sync
    $o = Other-Pc $h "ls"
    Sess $o "smash-64" @("--write", "battery/ssb64.sra", $big) | Out-Null          # smash arrives by pull
    Sess $o "zelda-ocarina" | Out-Null
    $ready = Join-Path $Scratch "hold.ready"; $stopf = Join-Path $Scratch "hold.stop"
    $ha = @("--data", $o.Data, "--vault", $o.Vault, "--url", $w.Base, "--uid", $o.Uid, "--secret", $o.Secret, "--name", $o.Name, "hold", "n64", "zelda-ocarina", $ready, $stopf)
    $hp = Start-Process $Tool -ArgumentList $ha -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch "hold.out")
    $script:holds += $hp.Id
    for ($i = 0; $i -lt 60 -and -not (Test-Path $ready); $i++) { Start-Sleep -Milliseconds 500 }
    Shot $h "saves_playing_elsewhere" $saves 20
    New-Item -ItemType File -Force $stopf | Out-Null
    $hp.WaitForExit(30000) | Out-Null

    # (6) paused (cloud_enabled false) and (7) claimed username header
    $h = New-Hub "paused" $w 4 @{ cloud_enabled = $false }
    Put $h "n64" "smash-64" "battery/ssb64.sra" $big
    Put $h "gba" "pokemon-emerald" "battery/emerald.sav" $big
    Shot $h "saves_paused" $saves 8
    $h = New-Hub "user" $w 5 @{ name = "QAPlayer"; username_claimed = $true }
    Put $h "n64" "smash-64" "battery/ssb64.sra" $big
    Shot $h "saves_username" $saves 12 @("1280x800")

    # (8) Settings > Account (seeded example code, not a real one)
    Shot $h "settings_account" @{ HUB_SHOT_PAGE = "settings"; HUB_SHOT_ACCOUNT = "code" } 4 @("1280x800", "1280x5600")
    Stop-Worker $w

    # (4) over quota: tiny caps, the older game's big state goes local-only
    $w2 = Start-Worker ($Port + 1) @("--account-cap", "400000", "--prot-cap", "200000")
    $h = New-Hub "over" $w2 0 @{}
    function Big($pc, $key, $rel, $bytes) {
        $p = Join-Path $pc.Vault "n64\$key\$rel"; New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
        $rng2 = New-Object System.Random $bytes; $buf = New-Object byte[] $bytes; $rng2.NextBytes($buf); [IO.File]::WriteAllBytes($p, $buf)
    }
    foreach ($g in @(@("old-game", "states/a.state", 200000), @("new-game", "states/b.state", 300000), @("huge-game", "states/c.state", 500000))) {
        Put $h "n64" $g[0] "battery/save.sra" "protected-save-$($g[0])"; Big $h $g[0] $g[1] $g[2]
    }
    Shot $h "saves_over_quota" $saves 30
    Stop-Worker $w2
} finally {
    foreach ($x in $script:holds) { Kill-Tree $x }
    foreach ($x in $script:workers) { Kill-Tree $x.Pid }
}
Write-Host "done: $Out"
