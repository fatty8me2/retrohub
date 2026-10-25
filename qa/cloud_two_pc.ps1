# Cloud saves v2: two-PC headless scenario (pull-before-launch, push-after-exit, offline play, retry, conflict, new PC restore).
# Two scratch data+vault dirs share one account on a LOCAL mock Worker (cloud_mock_server.exe, loopback only). Nothing visible,
# nothing from the user's install or saves is touched. Kills only the PIDs it started.
#   powershell -ExecutionPolicy Bypass -File qa\cloud_two_pc.ps1 [-Port 10150] [-BuildDir build\bin\Release]
#   -Real: run against the REAL local Cloudflare Worker (wrangler dev through server\cloudflare\scripts\qa_worker.mjs of the cloud-int-bs
#   worktree; ports 10400-10449, isolated registry, state under C:\rhq\<port>) instead of the C++ mock: qa\cloud_two_pc_real.ps1.
#   -RealPort 10410  -ServerDir <...\server\cloudflare>
param([int]$Port = 10150, [string]$BuildDir = "", [switch]$Real, [int]$RealPort = 10410, [string]$ServerDir = "")
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build\bin\Release" }
$Mock = Join-Path $BuildDir "cloud_mock_server.exe"
$Tool = Join-Path $BuildDir "qa_cloud_tool.exe"
foreach ($f in @($(if ($Real) { $Tool } else { $Mock }), $Tool)) { if (-not (Test-Path $f)) { throw "missing $f (build targets cloud_mock_server and qa_cloud_tool)" } }

$Scratch = Join-Path $env:TEMP "rhq_cloud_two_pc"
if (Test-Path $Scratch) { Remove-Item $Scratch -Recurse -Force }
New-Item -ItemType Directory -Force $Scratch | Out-Null
$alpha = "abcdefghijklmnopqrstuvwxyz234567".ToCharArray()
$rng = New-Object System.Random
function B32([int]$n) { -join (1..$n | ForEach-Object { $alpha[$rng.Next(32)] }) }
$Uid = B32 26
$Secret = B32 52          # never printed
$Url = "http://127.0.0.1:$Port"
$script:fail = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what"; $script:fail++ }
}
function Pc([string]$name) {
    $d = Join-Path $Scratch $name
    New-Item -ItemType Directory -Force (Join-Path $d "data") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $d "vault") | Out-Null
    @{ Name = $name; Data = (Join-Path $d "data"); Vault = (Join-Path $d "vault") }
}
function Run($pc, [string[]]$cmd, [string]$url = $Url) {
    $a = @("--data", $pc.Data, "--vault", $pc.Vault, "--url", $url, "--uid", $Uid, "--secret", $Secret, "--name", $pc.Name) + $cmd
    $out = & $Tool @a 2>&1 | Out-String
    $out.Trim().Replace("`r", " ").Replace("`n", " | ")
}
function Save($pc) { Join-Path $pc.Vault "n64\smash-64\battery\ssb64.sra" }
function Put($pc, [string]$text) {
    $p = Save $pc
    New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
    [IO.File]::WriteAllText($p, $text)
}
function Get-Text($pc) { $p = Save $pc; if (Test-Path $p) { [IO.File]::ReadAllText($p) } else { "" } }
function Ctl([string]$q) { Invoke-WebRequest -UseBasicParsing "$Url/__ctl?$q" | Out-Null }
function Session($pc, [string[]]$extra = @(), [string]$url = $Url) { Run $pc (@("session", "n64", "smash-64") + $extra) $url }

if ($Real) { . (Join-Path $PSScriptRoot "cloud_two_pc_real.ps1"); exit $script:fail }   # real local Worker scenarios (never returns)
$srv = Start-Process $Mock -ArgumentList @("--port", $Port, "--account", "$Uid`:$Secret") -PassThru -WindowStyle Hidden
try {
    for ($i = 0; $i -lt 40; $i++) { try { Ctl "delay=0"; break } catch { Start-Sleep -Milliseconds 250 } }
    $pc1 = Pc "pc1"; $pc2 = Pc "pc2"

    Write-Host "1. PC1 plays and exits: push after exit"
    Put $pc1 ("progress-" + ("A" * 3000))
    $r = Session $pc1
    Check (($r -match "action=proceed") -and ($r -match "state=Synced")) "empty cloud: proceed, then Synced after exit ($r)"

    Write-Host "2. PC2 launches the same game: pull before launch"
    $r = Session $pc2
    Check (($r -match "action=proceed") -and ($r -match "pulled=[1-9]")) "PC2 pulled the save ($r)"
    Check ((Get-Text $pc2) -eq (Get-Text $pc1)) "PC2 has PC1's save, byte for byte"

    Write-Host "3. offline: play at once, push later"
    $t0 = Get-Date
    $r = Session $pc2 @("--write", "battery/ssb64.sra", ("progress-" + ("B" * 3000))) "http://127.0.0.1:1"
    $el = ((Get-Date) - $t0).TotalSeconds
    Check (($r -match "action=proceed") -and ($r -match "offline=1")) "dead server: the game starts offline ($r)"
    Check ($el -lt 15) "offline session did not block ($([int]$el)s)"
    Check ((Get-Text $pc2) -like "progress-BBB*") "the save was written locally"
    Ctl "fail=503&failn=1000"
    $r = Session $pc2 @("--write", "battery/ssb64.sra", ("progress-" + ("B" * 3000)))
    Check ($r -match "state=Waiting") "server error: game waits in the queue ($r)"
    Ctl "failn=0"
    $r = Run $pc2 @("flush"); Check ($r -match "queued=0") "queue drained after the server is back ($r)"
    $r = Session $pc1; Check ($r -match "pulled=[1-9]") "PC1 pulls what PC2 pushed ($r)"
    Check ((Get-Text $pc1) -eq (Get-Text $pc2)) "both PCs agree"

    Write-Host "4. conflict: both PCs changed the save"
    $r = Session $pc1 @("--write", "battery/ssb64.sra", ("progress-" + ("C" * 3000)))
    Put $pc2 ("progress-" + ("D" * 3000))
    $r = Session $pc2 @("--resolve", "local")
    Check ($r -match "action=conflict") "conflict detected before launch ($r)"
    Check ($r -match "resolved=local") "keep this PC resolves ($r)"
    Check ((Get-Text $pc2) -like "progress-DDD*") "this PC's save is still the one in the vault"
    Check ((Get-ChildItem (Join-Path $pc2.Vault "_conflicts") -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count -ge 1) "the loser is kept in _conflicts"
    $r = Session $pc1; Check ($r -match "pulled=[1-9]") "PC1 receives the kept version ($r)"
    Check ((Get-Text $pc1) -like "progress-DDD*") "kept version is PC2's"

    Write-Host "5. a brand new PC (account added): cloud wins, recovery code round-trips"
    $pc3 = Pc "pc3"
    $r = Run $pc3 @("flush"); Check ($r -match "online=1") "new PC restored ($r)"
    Check ((Get-Text $pc3) -like "progress-DDD*") "new PC got the saves without any prompt"
    $r = Run $pc3 @("recovery"); Check (($r -match "roundtrip=1") -and ($r -notmatch $Secret)) "recovery code encodes/decodes, secret not printed"
} finally {
    if ($srv -and -not $srv.HasExited) { & taskkill /PID $srv.Id /T /F | Out-Null }
}
Write-Host ""
if ($script:fail) { Write-Host "$($script:fail) check(s) FAILED"; exit 1 }
Write-Host "cloud_two_pc: all checks passed"
