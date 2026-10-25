# Drives the username / account-code / friends flows THROUGH THE HUB (headless screenshot mode, SDL dummy video, nothing visible, muted)
# against a LOCAL real Worker (wrangler dev via qa_worker_shots.mjs: isolated registry, short --persist-to C:\rhq\<port>, loopback only).
# The hub runs the scripted flows from src\hub\userflow.cpp (HUB_USERFLOW) which call the same functions the buttons call.
# Never deploys, never production, never touches the user's install. Kills only the PIDs it started.
#   powershell -ExecutionPolicy Bypass -File qa\username_flows.ps1 [-Port 10468] [-Out qa\out\username_flows] [-ServerDir <...\server\cloudflare>]
param([int]$Port = 10468, [string]$Out = "", [string]$ServerDir = "", [string]$BuildDir = "", [int]$UpTo = 99)   # -UpTo N stops after phase N (debugging)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $PSScriptRoot "out\username_flows" }
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build\bin\Release" }
if (-not $ServerDir) { $ServerDir = Join-Path (Split-Path -Parent $Root) "cloud-int-bs\server\cloudflare" }
if ($Port -lt 10450 -or $Port -gt 10478) { throw "Port must be 10450-10478" }
$Tool = Join-Path $BuildDir "qa_cloud_tool.exe"
$Scratch = Join-Path $env:TEMP "rhq_username_flows"
if (Test-Path $Scratch) { Remove-Item $Scratch -Recurse -Force }
New-Item -ItemType Directory -Force $Scratch, $Out | Out-Null
Get-ChildItem $Out -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$env:RETROHUB_NO_SHELL = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:SSB64_MUTE = "1"
$env:QA_SERVER_DIR = $ServerDir
$script:workers = @()
$script:fails = 0
$Ver = (Get-Content (Join-Path $Root "VERSION") -Raw).Trim()

function Kill-Tree([int]$id) { cmd /c "taskkill /PID $id /T /F >nul 2>&1" | Out-Null }
function Start-Worker([int]$port) {
    $json = Join-Path $Scratch "worker$port.json"; $stop = Join-Path $Scratch "worker$port.stop"
    $a = @((Join-Path $PSScriptRoot "qa_worker_shots.mjs"), "--port", $port, "--out", $json, "--stop-file", $stop, "--parent-pid", $PID, "--accounts", 3, "--log", (Join-Path $Out "worker.log"))
    $p = Start-Process node -ArgumentList $a -WorkingDirectory $ServerDir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $Scratch "worker$port.out") -RedirectStandardError (Join-Path $Scratch "worker$port.err")
    $script:workers += @{ Pid = $p.Id; Stop = $stop }
    for ($i = 0; $i -lt 300 -and -not (Test-Path $json); $i++) { if ($p.HasExited) { break }; Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $json)) { throw "worker did not start (see $Scratch\worker$port.err)" }
    $j = Get-Content $json -Raw | ConvertFrom-Json
    @{ Base = $j.base; Accts = $j.accounts; Pid = $p.Id; Stop = $stop }
}
function Stop-Worker($w) { New-Item -ItemType File -Force $w.Stop | Out-Null; Start-Sleep -Seconds 3; Kill-Tree $w.Pid }

# A "PC": its own copy of the hub exe + userdata (data dir) + vault. $acct = worker account index to start as, or -1 for a fresh PC (no identity).
function New-Pc([string]$name, $w, [int]$acct, [hashtable]$settings = @{}) {
    $d = Join-Path $Scratch $name
    New-Item -ItemType Directory -Force $d | Out-Null
    foreach ($f in @("RetroHub.exe", "SDL2.dll")) { Copy-Item (Join-Path $BuildDir $f) $d }
    foreach ($dir in @("data", "controls")) { if (Test-Path (Join-Path $BuildDir $dir)) { Copy-Item (Join-Path $BuildDir $dir) (Join-Path $d $dir) -Recurse } }
    $ud = Join-Path $d "userdata"; New-Item -ItemType Directory -Force (Join-Path $ud "saves") | Out-Null
    $s = [ordered]@{ server = ($w.Base -replace "^http", "ws"); name = $name; username_claimed = $false; cloud_enabled = $true; setup_done = $true
                     find_games_done = $true; changelog_seen = $Ver; sync_card_dismissed = $true }
    foreach ($k in $settings.Keys) { $s[$k] = $settings[$k] }
    [IO.File]::WriteAllText((Join-Path $ud "hub.json"), ($s | ConvertTo-Json))
    $pc = @{ Name = $name; Dir = $d; Data = $ud; Vault = (Join-Path $ud "saves"); W = $w }
    if ($acct -ge 0) {
        $idj = '{"user_id":"' + $w.Accts[$acct].uid + '","secret":"' + $w.Accts[$acct].secret + '","created":1}'
        New-Item -ItemType Directory -Force (Join-Path $ud "saves\_identity") | Out-Null
        [IO.File]::WriteAllText((Join-Path $ud "identity.json"), $idj); [IO.File]::WriteAllText((Join-Path $ud "saves\_identity\identity.json"), $idj)
    }
    $pc
}

# Runs one scripted flow in the hub. Returns the log text. Counts FAIL lines / a missing DONE as failures.
function Run-Flow($pc, [string]$tag, [string]$flow, [hashtable]$vars, [switch]$NoWait) {
    $log = Join-Path $Out "$tag.log"; Remove-Item $log -ErrorAction SilentlyContinue
    $bmp = Join-Path $Out "$tag.bmp"
    $set = @{ HUB_SCREENSHOT = $bmp; HUB_SHOT_NET = "1"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "1"; HUB_SHOT_CLOUD_REAL = "1"; HUB_SHOT_SIZE = "1280x800"
              HUB_USERFLOW = $flow; HUB_USERFLOW_LOG = $log; HUB_SHOT_TRACE = (Join-Path $Out "$tag.trace") } + $vars
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $p = Start-Process -FilePath (Join-Path $pc.Dir "RetroHub.exe") -WorkingDirectory $pc.Dir -PassThru -WindowStyle Hidden
    foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if ($NoWait) { return $p }
    Finish-Flow $p $tag
}
function Finish-Flow($p, [string]$tag) {
    $log = Join-Path $Out "$tag.log"
    if (-not $p.WaitForExit(150000)) { Kill-Tree $p.Id; Add-Content $log "FAIL hub did not exit in time (killed)" }
    Get-ChildItem $Out -Filter "$tag*.bmp" | ForEach-Object {
        $im = [System.Drawing.Image]::FromFile($_.FullName); $im.Save(($_.FullName -replace "\.bmp$", ".png"), [System.Drawing.Imaging.ImageFormat]::Png); $im.Dispose(); Remove-Item $_.FullName
    }
    $text = if (Test-Path $log) { Get-Content $log -Raw } else { "FAIL no log written (hub crashed or flow never started)" }
    $bad = ([regex]::Matches($text, "(?m)^(\[.*?\] )?FAIL")).Count
    $done = $text -match "DONE pass="
    if ($bad -gt 0 -or -not $done) { $script:fails++ }
    Write-Host ("{0,-22} {1}  ({2} FAIL line(s), DONE={3})" -f $tag, $(if ($bad -eq 0 -and $done) { "PASS" } else { "FAIL" }), $bad, $done)
    $text
}
function Show($text) { $text -split "`n" | Where-Object { $_ -match "probe|claim|PASS|FAIL|signed|incoming|friend|code|prompt|DONE|connected" } | ForEach-Object { Write-Host "    $_" } }

try {
    $w = Start-Worker $Port
    Write-Host "worker $($w.Base) pid $($w.Pid)"

    # (1) the neighbour who already owns zed_kid (wizard Name step, fresh PC)
    $z = New-Pc "zed" $w -1
    Show (Run-Flow $z "1_zed_claims" "claim" @{ HUB_SHOT_WIZARD = "name"; UF_CLAIM = "zed_kid" })

    if ($UpTo -lt 2) { throw "stopped by -UpTo" }
    # (2) wizard claim, fresh PC: not allowed (reserved) / taken / invalid / available -> claim -> wizard moves on
    $a = New-Pc "alice" $w -1
    Show (Run-Flow $a "2_alice_wizard_claim" "claim" @{ HUB_SHOT_WIZARD = "name"; UF_PROBE = "admin,zed_kid,a"; UF_EXPECT = "admin=NotAllowed,zed_kid=Taken,a=Invalid"; UF_CLAIM = "alicia" })
    $id = Get-Content (Join-Path $a.Data "identity.json") -Raw | ConvertFrom-Json

    # alice's cloud save (pushed as another PC of her account through the real client engine)
    $o = Join-Path $Scratch "alice_other"; New-Item -ItemType Directory -Force (Join-Path $o "data"), (Join-Path $o "vault") | Out-Null
    $big = "progress-" + ("A" * 3000)
    $ta = @("--data", (Join-Path $o "data"), "--vault", (Join-Path $o "vault"), "--url", $w.Base, "--uid", $id.user_id, "--secret", $id.secret, "--name", "Alice-Laptop", "session", "n64", "zelda-ocarina", "--write", "battery/zelda.sra", $big)
    Write-Host ("    cloud seed: " + ((& $Tool @ta 2>&1 | Out-String).Trim().Replace("`n", " | ")))

    if ($UpTo -lt 3) { throw "stopped by -UpTo" }
    # (3) alice makes her permanent account code (Settings > Account)
    $codeFile = Join-Path $Scratch "alice_code.txt"
    Show (Run-Flow $a "3_alice_account_code" "makecode" @{ HUB_SHOT_PAGE = "settings"; UF_OUT = $codeFile })

    if ($UpTo -lt 4) { throw "stopped by -UpTo" }
    # (4) returning player on a 2nd scratch PC: wizard "I already have a username on another PC" -> code -> signed in as alicia -> cloud save pulled
    $b = New-Pc "bob_pc" $w -1
    $pulled = Join-Path $b.Vault "n64\zelda-ocarina\battery\zelda.sra"
    Show (Run-Flow $b "4_returning_player" "returning" @{ HUB_SHOT_WIZARD = "returning"; UF_CODE_FILE = $codeFile; UF_FILE = $pulled; UF_FILE_PREFIX = "progress-AAAA" })

    if ($UpTo -lt 5) { throw "stopped by -UpTo" }
    # (5) add friend by username: carol claims her name, then adds alicia; alice accepts
    $c = New-Pc "carol" $w -1
    Show (Run-Flow $c "5a_carol_claims" "claim" @{ HUB_SHOT_WIZARD = "name"; UF_CLAIM = "carolyn" })
    Show (Run-Flow $c "5b_carol_adds_alice" "addfriend" @{ HUB_SHOT_FRIENDS = "1"; UF_TARGET = "alicia" })
    Show (Run-Flow $a "5c_alice_accepts" "accept" @{ HUB_SHOT_FRIENDS = "1"; UF_TARGET = "carolyn" })

    if ($UpTo -lt 6) { throw "stopped by -UpTo" }
    # (6) needs_new_name prompt: an install from before usernames (identity already on disk, never named) whose old display name is now
    #     an explicit claim of somebody else ("alicia") -> the server flags it needs_new_name -> the prompt appears by itself.
    $d = New-Pc "alicia" $w -1
    $B32 = "abcdefghijklmnopqrstuvwxyz234567"
    $ouid = -join (1..26 | ForEach-Object { $B32[(Get-Random -Maximum 32)] }); $osec = -join (1..52 | ForEach-Object { $B32[(Get-Random -Maximum 32)] })
    [IO.File]::WriteAllText((Join-Path $d.Data "identity.json"), '{"user_id":"' + $ouid + '","secret":"' + $osec + '","created":1}')
    Show (Run-Flow $d "6_needs_new_name_prompt" "prompt" @{ UF_CLAIM = "davey_j" })

    # (7) change username with the hold: alice changes alicia -> alicia_two while zed (already running) probes the old name right after
    #     (held for UNAME_HOLD_MS=4000 from the worker's .dev.vars), and again after the hold (free).
    $sig = Join-Path $Scratch "changed.flag"
    $pz = Run-Flow $z "7b_zed_probes_old_name" "hold" @{ UF_NAME = "alicia"; UF_WAIT_FILE = $sig; UF_SLEEP_MS = "5200"; UF_EXPECT_NOW = "Taken" } -NoWait
    Show (Run-Flow $a "7a_alice_changes_name" "change" @{ HUB_SHOT_PAGE = "settings"; UF_CLAIM = "alicia_two"; UF_SIGNAL = $sig })
    Show (Finish-Flow $pz "7b_zed_probes_old_name")
}
catch { if ("$_" -ne "stopped by -UpTo") { throw } }
finally {
    foreach ($x in $script:workers) { try { New-Item -ItemType File -Force $x.Stop | Out-Null } catch { } }
    Start-Sleep -Seconds 3
    foreach ($x in $script:workers) { Kill-Tree $x.Pid }
}
Write-Host ""
Write-Host $(if ($script:fails -eq 0) { "ALL USERNAME FLOWS PASSED" } else { "$($script:fails) FLOW(S) FAILED  (logs + screenshots: $Out)" })
exit $script:fails
