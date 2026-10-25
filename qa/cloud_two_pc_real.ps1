# Dot-sourced by cloud_two_pc.ps1 -Real. Runs the cloud saves v2 client engine against the REAL local Cloudflare Worker
# (wrangler dev, isolated registry, state under C:\rhq\<port>, loopback only). Starts only its own PIDs and kills them by PID tree.
if ($RealPort -lt 10400 -or $RealPort -gt 10449) { throw "RealPort must be 10400-10449" }
if (-not $ServerDir) { $ServerDir = Join-Path (Split-Path -Parent $Root) "cloud-int-bs\server\cloudflare" }
if (-not (Test-Path (Join-Path $ServerDir "scripts\qa_worker.mjs"))) { throw "missing qa_worker.mjs in $ServerDir" }
$env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"
$script:workers = @()
$script:holds = @()

function Start-Worker([int]$port, [string[]]$extra = @()) {
    $json = Join-Path $Scratch "worker$port.json"; $stop = Join-Path $Scratch "worker$port.stop"; $log = Join-Path $Scratch "worker$port.log"
    foreach ($f in @($json, $stop)) { if (Test-Path $f) { Remove-Item $f -Force } }
    $a = @("scripts/qa_worker.mjs", "--port", $port, "--out", $json, "--stop-file", $stop, "--parent-pid", $PID, "--log", $log, "--accounts", 4) + $extra
    $p = Start-Process node -ArgumentList $a -WorkingDirectory $ServerDir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $Scratch "worker$port.out") -RedirectStandardError (Join-Path $Scratch "worker$port.err")
    $script:workers += @{ Pid = $p.Id; Stop = $stop }
    for ($i = 0; $i -lt 300 -and -not (Test-Path $json); $i++) { if ($p.HasExited) { break }; Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $json)) { throw "real worker did not start (see $Scratch\worker$port.out/.err)" }
    $j = Get-Content $json -Raw | ConvertFrom-Json
    @{ Base = $j.base; Accts = $j.accounts; Log = $log; Proc = $p; Pid = $p.Id; Stop = $stop }
}
function Stop-Worker($w) {
    New-Item -ItemType File -Force $w.Stop | Out-Null
    Start-Sleep -Seconds 3
    Kill-Tree $w.Pid
}
function Kill-Tree([int]$id) { cmd /c "taskkill /PID $id /T /F >nul 2>&1" | Out-Null }
# one account per scenario group: the Worker rate-limits status/manifest/lease to 60 per minute per account
function Use-Worker($w, [int]$i = 0) { $script:Url = $w.Base; $script:Uid = $w.Accts[$i].uid; $script:Secret = $w.Accts[$i].secret }
function LogHas($w, [string]$pat) { Start-Sleep -Seconds 2; (Test-Path $w.Log) -and ((Get-Content $w.Log -Raw) -match $pat) }
function GFile($pc, [string]$key, [string]$rel) { Join-Path $pc.Vault ("n64\" + $key + "\" + $rel.Replace("/", "\")) }
function GText($pc, [string]$key, [string]$rel) { $p = GFile $pc $key $rel; if (Test-Path $p) { [IO.File]::ReadAllText($p) } else { "" } }
function GPut($pc, [string]$key, [string]$rel, [string]$text) {
    $p = GFile $pc $key $rel; New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null; [IO.File]::WriteAllText($p, $text)
}
function Sess($pc, [string]$key, [string[]]$extra = @(), [string]$url = $Url) { Run $pc (@("session", "n64", $key) + $extra) $url }
function Ver-Dump($pc, [string]$key) { Run $pc @("versions", "n64", $key) }

try {
    $w = Start-Worker $RealPort
    Use-Worker $w
    Write-Host "real worker: $($w.Base) (pid $($w.Pid))"
    $pc1 = Pc "pc1"; $pc2 = Pc "pc2"; $pc3 = Pc "pc3"
    $G = "smash-64"; $SaveRel = "battery/ssb64.sra"

    Write-Host "R1. first launch, empty cloud: push after exit"
    GPut $pc1 $G $SaveRel ("progress-" + ("A" * 3000))
    $r = Sess $pc1 $G
    Check (($r -match "action=proceed") -and ($r -match "state=Synced") -and ($r -match "cloud_bytes=[1-9]")) "PC1: proceed, then Synced with bytes in the cloud ($r)"
    Check (LogHas $w "/saves/v2/commit") "wrangler log shows the commit request"

    Write-Host "R2. PC2 pulls before launch"
    $r = Sess $pc2 $G
    Check (($r -match "action=proceed") -and ($r -match "pulled=[1-9]")) "PC2 pulled ($r)"
    Check ((GText $pc2 $G $SaveRel) -eq (GText $pc1 $G $SaveRel)) "byte for byte"

    Write-Host "R3. offline play, then reconnect: queue drains"
    $t0 = Get-Date
    $r = Sess $pc2 $G @("--write", $SaveRel, ("progress-" + ("B" * 3000))) "http://127.0.0.1:1"
    Check (($r -match "offline=1") -and ($r -match "state=Waiting") -and (((Get-Date) - $t0).TotalSeconds -lt 20)) "dead server: starts offline quickly, game Waiting ($r)"
    Check ((GText $pc2 $G $SaveRel) -like "progress-BBB*") "save written locally"
    $r = Run $pc2 @("flush"); Check (($r -match "online=1") -and ($r -match "queued=0")) "reconnected: queue drained ($r)"
    $r = Sess $pc1 $G; Check (($r -match "pulled=[1-9]") -and ((GText $pc1 $G $SaveRel) -like "progress-BBB*")) "PC1 gets PC2's offline progress ($r)"

    Write-Host "R4a. conflict: keep this PC"
    Sess $pc1 $G @("--write", $SaveRel, ("progress-" + ("C" * 3000))) | Out-Null
    GPut $pc2 $G $SaveRel ("progress-" + ("D" * 3000))
    $r = Sess $pc2 $G @("--resolve", "local")
    Check (($r -match "action=conflict") -and ($r -match "resolved=local") -and ($r -match "state=Synced")) "conflict, keep this PC, Synced ($r)"
    Check ((GText $pc2 $G $SaveRel) -like "progress-DDD*") "PC2 keeps its save"
    $r = Sess $pc1 $G; Check ((GText $pc1 $G $SaveRel) -like "progress-DDD*") "PC1 receives the kept version ($r)"
    Write-Host "R4b. conflict: use cloud"
    Sess $pc1 $G @("--write", $SaveRel, ("progress-" + ("E" * 3000))) | Out-Null
    GPut $pc2 $G $SaveRel ("progress-" + ("F" * 3000))
    $r = Sess $pc2 $G @("--resolve", "cloud")
    Check (($r -match "action=conflict") -and ($r -match "resolved=cloud")) "conflict, use cloud ($r)"
    Check ((GText $pc2 $G $SaveRel) -like "progress-EEE*") "PC2 now has the cloud version"
    Check ((Get-ChildItem (Join-Path $pc2.Vault "_conflicts") -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count -ge 1) "the replaced local save is kept in _conflicts"
    Write-Host "R4c. conflict: keep both / newer wins"
    Start-Sleep -Seconds 1
    Sess $pc1 $G @("--write", $SaveRel, ("progress-" + ("G" * 3000))) | Out-Null
    Start-Sleep -Seconds 2
    GPut $pc2 $G $SaveRel ("progress-" + ("H" * 3000))
    $r = Sess $pc2 $G @("--resolve", "newer")
    Check ($r -match "action=conflict") "conflict detected ($r)"
    Check ((GText $pc2 $G $SaveRel) -like "progress-HHH*") "newer (PC2's) wins locally"
    $r = Sess $pc1 $G; Check ((GText $pc1 $G $SaveRel) -like "progress-HHH*") "PC1 converges on the newer one ($r)"

    Write-Host "R5. restore previous version (account 2)"
    Use-Worker $w 1; $pc1 = Pc "r1"; $pc2 = Pc "r2"; $pc3 = Pc "r3"
    $RG = "restore-game"
    foreach ($t in @("1", "2", "3")) { Sess $pc1 $RG @("--write", $SaveRel, ("v" + $t + "-" + ("R" * 2000))) | Out-Null; Start-Sleep -Seconds 1 }
    $v = Ver-Dump $pc1 $RG
    Check (($v -match "ver=1") -and ($v -match "ver=3")) "history lists the versions ($v)"
    $r = Run $pc1 @("restore", "n64", $RG, $SaveRel, "1")
    Check (($r -match "restore ok=1")) "restore reported ok ($r)"
    Check ((GText $pc1 $RG $SaveRel) -like "v1-*") "the old version is back in the vault"
    $r = Sess $pc2 $RG; Check ((GText $pc2 $RG $SaveRel) -like "v1-*") "PC2 pulls the restored version ($r)"

    Write-Host "R6. guest online session never uploads (account 3)"
    Use-Worker $w 2; $pc1 = Pc "g1"; $pc2 = Pc "g2"; $pc3 = Pc "g3"
    GPut $pc1 $G $SaveRel ("progress-" + ("A" * 3000)); Sess $pc1 $G | Out-Null
    $before = Ver-Dump $pc1 $G
    $r = Sess $pc2 $G @("--readonly", "--write", $SaveRel, "GUEST-SAVE-DO-NOT-UPLOAD")
    $after = Ver-Dump $pc1 $G
    Check ($before -eq $after) "cloud versions unchanged after a read-only session ($after)"
    $r = Sess $pc1 $G; Check ((GText $pc1 $G $SaveRel) -notlike "GUEST*") "PC1 sees no guest data ($r)"
    $r = Run $pc3 @("flush"); Check ((GText $pc3 $G $SaveRel) -notlike "GUEST*") "a fresh PC restores without guest data"

    Write-Host "R7. other PC holds the lease (account 4)"
    Use-Worker $w 3; $pc1 = Pc "l1"; $pc2 = Pc "l2"
    GPut $pc1 $G $SaveRel ("progress-" + ("A" * 3000)); Sess $pc1 $G | Out-Null; Sess $pc2 $G | Out-Null
    $ready = Join-Path $Scratch "hold.ready"; $stopf = Join-Path $Scratch "hold.stop"
    foreach ($f in @($ready, $stopf)) { if (Test-Path $f) { Remove-Item $f -Force } }
    $ha = @("--data", $pc1.Data, "--vault", $pc1.Vault, "--url", $Url, "--uid", $Uid, "--secret", $Secret, "--name", "pc1", "hold", "n64", $G, $ready, $stopf)
    $hp = Start-Process $Tool -ArgumentList $ha -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Scratch "hold.out")
    $script:holds += $hp.Id
    for ($i = 0; $i -lt 60 -and -not (Test-Path $ready); $i++) { Start-Sleep -Milliseconds 500 }
    Check (Test-Path $ready) "PC1 is in a running session"
    $r = Sess $pc2 $G @("--lease", "cancel")
    Check (($r -match "prelaunch action=lease") -and ($r -match "held_by=l1") -and ($r -match "state=PlayingElsewhere")) "PC2 pre-launch: NeedLeaseDecision, held by l1, Cancel leaves the game not started ($r)"
    $r = Sess $pc2 $G @("--lease", "take")
    Check (($r -match "lease_choice=take -> action=proceed")) "take over proceeds ($r)"
    $r = Sess $pc2 $G @("--lease", "play")
    Check (($r -match "action=proceed")) "second PC2 session (play anyway / lease now ours) proceeds ($r)"
    New-Item -ItemType File -Force $stopf | Out-Null
    $hp.WaitForExit(30000) | Out-Null
    Check (LogHas $w "/saves/v2/lease") "wrangler log shows lease requests"

    Write-Host "R10. fresh PC via the account code (XXXX-XXXX-XXXX), account 2"
    Use-Worker $w 2
    $a1 = Pc "a1"
    $CG = "code-game"; GPut $a1 $CG $SaveRel ("progress-" + ("Z" * 3000)); Sess $a1 $CG | Out-Null
    $r = Run $a1 @("recovery")
    $m = [regex]::Match($r, "account_code=([A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4})")
    Check ($m.Success -and $r -match "made=1") "the account code is made on the server ($($r -replace 'account_code=\S+','account_code=<hidden>'))"
    $code = $m.Groups[1].Value
    $a2 = Pc "a2"
    $fakeUid = (B32 26); $fakeSecret = (B32 52)
    $ad = @("--data", $a2.Data, "--vault", $a2.Vault, "--url", $Url, "--uid", $fakeUid, "--secret", $fakeSecret, "--name", "a2")
    $r = (& $Tool @ad adopt "ABCD-EFGH-JKMN" 2>&1 | Out-String).Trim()
    Check ($r -match "adopt ok=0") "a wrong code is refused ($r)"
    $r = (& $Tool @ad adopt $code 2>&1 | Out-String).Trim()
    Check ($r -match "adopt ok=1") "the right code adopts the account ($r)"
    $idtxt = Get-Content (Join-Path $a2.Data "identity.json") -Raw
    Check ($idtxt -match $Uid) "identity.json on the new PC now holds the account (uid match)"
    $r = Run $a2 @("flush")
    Check ((GText $a2 $CG $SaveRel) -like "progress-ZZZ*") "the new PC got the saves after adopting ($r)"
    $r = (& $Tool @ad adopt ($code.ToLower().Replace("-", " ")) 2>&1 | Out-String).Trim()
    Check ($r -match "adopt ok=1") "the code is not single-use and accepts lower case / spaces ($r)"

    Write-Host "R8. server log sanity"
    $lg = (Get-Content $w.Log | Where-Object { $_ -notmatch "/link/redeem" }) -join "`n"   # the deliberate wrong-code redeem is a 404
    Check (($lg -match "/saves/v2/manifest") -and ($lg -match "/saves/v2/plan") -and ($lg -match "/saves/v2/blob")) "manifest, plan and blob requests reached the Worker"
    Check ($lg -notmatch " 40[0134] ") "no 400/401/403/404 replies in the Worker log"
    Stop-Worker $w

    Write-Host "R9. over quota (tiny caps: account 400000, protected 200000)"
    $w2 = Start-Worker ($RealPort + 1) @("--account-cap", "400000", "--prot-cap", "200000")
    Use-Worker $w2 0
    $q1 = Pc "q1"; $q2 = Pc "q2"
    $r = Sess $q1 "old-game" @("--write", $SaveRel, "old-protected-save", "--writebig", "states/a.state", "200000")
    Check ($r -match "state=Synced") "old game fits ($r)"
    Start-Sleep -Seconds 2
    $r = Sess $q1 "new-game" @("--write", $SaveRel, "new-protected-save", "--writebig", "states/b.state", "300000")
    Check ($r -match "state=") "new game pushed ($r)"
    $r = Run $q1 @("prelaunch", "n64", "old-game")
    Check (($r -match "files_local_only=[1-9]") -or ($r -match "local_only=OverCap")) "oldest game's large state went local-only ($r)"
    Check (Test-Path (GFile $q1 "old-game" "states/a.state")) "its state file is still on this PC"
    $r = Sess $q2 "old-game"
    Check (($r -match "pulled=[1-9]") -and ((GText $q2 "old-game" $SaveRel) -eq "old-protected-save") -and -not (Test-Path (GFile $q2 "old-game" "states/a.state"))) "protected save still syncs, evicted state is not in the cloud ($r)"
    $r = Sess $q1 "huge-game" @("--write", $SaveRel, "huge-protected", "--writebig", "states/c.state", "500000")
    $r2 = $r
    Check (($r2 -match "files_local_only=[1-9]") -and ($r2 -match "files_synced=[1-9]")) "state bigger than the whole account stays local, protected save synced ($r2)"
    Stop-Worker $w2
} finally {
    foreach ($h in $script:holds) { Kill-Tree $h }
    foreach ($x in $script:workers) { Kill-Tree $x.Pid }
}
Write-Host ""
if ($script:fail) { Write-Host "$($script:fail) check(s) FAILED"; exit 1 }
Write-Host "cloud_two_pc -Real: all checks passed"
exit 0
