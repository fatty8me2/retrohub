# Real Lichii install / upgrade QA (downloads the pinned build from github.com/zortos293/Lichii; launches nothing, opens no window).
# Runs RetroHub's own job through hub_lichii_upgrade_qa.exe on COPIES of a build-88 install and checks four scenarios:
#   1. upgrade 88 -> the pinned build 125 (user\ byte-identical, program files match the pinned-build reference, no scratch left)
#   2. second run is a no-op (no download, nothing changes)
#   3. fresh install into an empty root
#   4. forced failure (<root>\_downloads is a FILE) keeps the old build and user\ intact
# Usage:
#   powershell -NoProfile -File qa\lichii_upgrade_real.ps1 -Bench <copy-of-a-build-88-lichii-folder> -Rx <pinned-build-reference-folder> -Work <fresh-folder>
# -Bench is only read (copied). -Work must be a new folder and must not be under the user's real data folder.
param(
    [string]$Tool = (Join-Path $PSScriptRoot '..\build\bin\Release\hub_lichii_upgrade_qa.exe'),
    [Parameter(Mandatory = $true)][string]$Bench,
    [Parameter(Mandatory = $true)][string]$Rx,
    [Parameter(Mandatory = $true)][string]$Work
)
$ErrorActionPreference = 'Continue'
$Sha88 = '3405860a13f7b0729548ddb9402a355f4cfb1cd6a6d705fb8ea26f88879b0407'
$ShaPin = 'c55eaaedbdcd87eebcab5510a3d19c49545a4718b9355d95150150d5aeb67bb6'   # build 125
$Excluded = @('lichii.pdb', 'eden-cli.pdb', 'vc_redist.x64.exe', 'lichii-updater.exe')

if ($Work -match 'smashClassic\\userdata') { Write-Error 'Refusing: -Work is under the real user data folder.'; exit 2 }
if (Test-Path -LiteralPath $Work) { Write-Error "-Work must be a fresh folder: $Work already exists."; exit 2 }
if (-not (Test-Path -LiteralPath (Join-Path $Bench 'lichii.exe'))) { Write-Error "-Bench has no lichii.exe: $Bench"; exit 2 }
$Tool = (Resolve-Path -LiteralPath $Tool).Path
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Bin = Join-Path $Work 'bin'
New-Item -ItemType Directory -Force -Path $Bin | Out-Null
Copy-Item -LiteralPath $Tool -Destination $Bin -Force
$Tool = Join-Path $Bin 'hub_lichii_upgrade_qa.exe'
$Log = Join-Path $Work 'qa_log.txt'
$script:Fails = 0
function Log([string]$m) { Add-Content -LiteralPath $Log -Value $m; Write-Host $m }
function Check([bool]$ok, [string]$what) { if ($ok) { Log "  PASS  $what" } else { Log "  FAIL  $what"; $script:Fails++ } }

# Sorted "sha256 size relpath" lines for every file under $dir (or $dir\user only).
function Snap([string]$dir, [string]$sub = '') {
    $base = if ($sub) { Join-Path $dir $sub } else { $dir }
    Get-ChildItem -LiteralPath $base -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
        '{0} {1} {2}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Length, $_.FullName.Substring($dir.Length + 1)
    }
}
function Program-Files([string]$dir) {
    Get-ChildItem -LiteralPath $dir -Force | Where-Object { $_.Name -ne 'user' } | ForEach-Object { $_.Name }
}
# Every reference file (except the three RetroHub drops) must exist with the same hash; the three must be absent.
function Check-Rx([string]$lic) {
    $bad = 0; $n = 0
    Get-ChildItem -LiteralPath $Rx -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($Rx.Length + 1)
        if ($Excluded -contains $_.Name) { if (Test-Path -LiteralPath (Join-Path $lic $rel)) { $bad++; Log "    present but must be absent: $rel" }; return }
        $n++
        $p = Join-Path $lic $rel
        if (-not (Test-Path -LiteralPath $p)) { $bad++; Log "    missing: $rel"; return }
        if ((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash) { $bad++; Log "    hash differs: $rel" }
    }
    Check ($bad -eq 0) "program files match the pinned-build reference ($n files, $($Excluded.Count) excluded absent)"
}
function Check-Clean([string]$root, [string]$lic) {
    Check (-not (Test-Path -LiteralPath (Join-Path $lic '_old'))) 'no _old left'
    Check (-not (Test-Path -LiteralPath (Join-Path $lic '_extract'))) 'no _extract left'
    Check (-not (Test-Path -LiteralPath (Join-Path $lic 'retrohub-upgrade.txt'))) 'no journal left'
    $dl = Join-Path $root '_downloads'
    $left = @(Get-ChildItem -LiteralPath $dl -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '\.zip(\.part)?$|\.part$' })
    Check ($left.Count -eq 0) '_downloads has no leftover zip or .part'
}
function Run-Tool([string]$root, [string]$logName) {
    $t0 = Get-Date
    $out = & $Tool $root 2>&1
    $code = $LASTEXITCODE
    $out | Out-File -FilePath (Join-Path $Work $logName) -Encoding utf8
    foreach ($line in $out) { Log "    $line" }
    Log ("    exit={0} secs={1}" -f $code, [int]((Get-Date) - $t0).TotalSeconds)
    return [int]$code
}
function New-Root88([string]$name) {
    $root = Join-Path $Work $name
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    Copy-Item -LiteralPath $Bench -Destination (Join-Path $root 'lichii') -Recurse -Force
    Set-Content -LiteralPath (Join-Path $root 'lichii\retrohub-install.txt') -Value "Lichii build-88`nsha256 $Sha88" -Encoding ascii
    return $root
}

Log "== Lichii real upgrade QA $(Get-Date -Format s) =="
# ---- Scenario 1
Log '== Scenario 1: upgrade 88 -> 125'
$r1 = New-Root88 's1'
$lic1 = Join-Path $r1 'lichii'
$userBefore = Snap $lic1 'user'
$progBefore = Program-Files $lic1
Log "  before: user files=$($userBefore.Count) program entries=$($progBefore.Count)"
$c = Run-Tool $r1 's1_tool.txt'
Check ($c -eq 0) 'exit 0'
$txt = Get-Content (Join-Path $Work 's1_tool.txt') -Raw
Check ($txt -match 'after: .*outdated=0') 'after shows outdated=0'
$marker = Get-Content -LiteralPath (Join-Path $lic1 'retrohub-install.txt')
Check (($marker -join "`n") -match "sha256 $ShaPin" -and ($marker -join "`n") -match 'build 125') 'marker has the pinned sha256 and mentions build 125'
$userAfter = Snap $lic1 'user'
Check (($userBefore -join "`n") -eq ($userAfter -join "`n")) "user\ byte-identical ($($userBefore.Count) files, none missing or added)"
Check-Rx $lic1
Check-Clean $r1 $lic1
$gone = @($progBefore | Where-Object { $_ -ne 'retrohub-install.txt' -and -not (Test-Path -LiteralPath (Join-Path $Rx $_)) -and (Test-Path -LiteralPath (Join-Path $lic1 $_)) })
Check ($gone.Count -eq 0) 'build-88 program files not in build 125 are gone'

# ---- Scenario 2: second run on the same root
Log '== Scenario 2: second run is a no-op'
$snapBefore2 = Snap $lic1
$c = Run-Tool $r1 's2_tool.txt'
Check ($c -eq 0) 'exit 0'
Check (-not ((Get-Content (Join-Path $Work 's2_tool.txt') -Raw) -match 'phase 2')) 'no download (no phase 2)'
Check (($snapBefore2 -join "`n") -eq ((Snap $lic1) -join "`n")) 'nothing changed (re-hashed every file)'

# ---- Scenario 3: fresh install
Log '== Scenario 3: fresh install into an empty root'
$r3 = Join-Path $Work 's3'
New-Item -ItemType Directory -Force -Path $r3 | Out-Null
$c = Run-Tool $r3 's3_tool.txt'
$lic3 = Join-Path $r3 'lichii'
Check ($c -eq 0) 'exit 0'
Check (Test-Path -LiteralPath (Join-Path $lic3 'user') -PathType Container) 'lichii\user exists'
Check-Rx $lic3
Check-Clean $r3 $lic3

# ---- Scenario 4: failure keeps the old build (_downloads is a FILE so the download cannot be written)
Log '== Scenario 4: forced failure keeps the old build'
$r4 = New-Root88 's4'
$lic4 = Join-Path $r4 'lichii'
Set-Content -LiteralPath (Join-Path $r4 '_downloads') -Value 'QA blocker' -Encoding ascii
$snapBefore4 = Snap $lic4
$userBefore4 = Snap $lic4 'user'
$c = Run-Tool $r4 's4_tool.txt'
$txt4 = Get-Content (Join-Path $Work 's4_tool.txt') -Raw
Check ($c -ne 0 -and $txt4 -match 'result: FAILED') 'tool reports FAILED'
Check (($snapBefore4 -join "`n") -eq ((Snap $lic4) -join "`n")) 'old build-88 files and marker untouched (re-hashed)'
Check (($userBefore4 -join "`n") -eq ((Snap $lic4 'user') -join "`n")) 'user\ byte-identical'
Check (-not (Test-Path -LiteralPath (Join-Path $lic4 '_old'))) 'no _old left'
Check (-not (Test-Path -LiteralPath (Join-Path $lic4 '_extract'))) 'no _extract left'
Check (-not (Test-Path -LiteralPath (Join-Path $lic4 'retrohub-upgrade.txt'))) 'no journal left'

Log "== DONE: $($script:Fails) failed check(s)"
exit ([int]($script:Fails -gt 0))
