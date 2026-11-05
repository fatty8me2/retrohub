# Wave 3 X3 (Pal Park / DS Slot-2): retrorun --slot2-rom / --slot2-save, headless, muted, offline, scratch dirs only. No game is ever started with a real DS ROM.
#   1. offline: the flags are accepted ("slot2: accepted" in the log); the run then stops at the (missing) core, exit 1, never "REFUSED"
#   2. --net, --ds-link, --rollback, --link, --share-host-save: refused with exit 2 and "slot2: REFUSED" (before any core is loaded)
#   3. --slot2-save alone and --slot2-rom on a GBA game: refused
#   4. (when the melonDS DS core + a test GBA ROM are around) the core's Slot-2 subsystems are listed and the right one is picked
#      with and without a save; the vault save is backed up (keep 3) and is byte-identical afterwards (a garbage DS ROM never loads).
# Exit 0 pass, 1 fail, 77 skipped. Usage: powershell -NoProfile -File qa\palpark_headless.ps1 [-Root <repo>]
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
$rr = Join-Path $Root 'build\bin\Release\retrorun.exe'
if (-not (Test-Path $rr)) { Write-Output "SKIP palpark_headless: $rr missing"; exit 77 }
$w = Join-Path $env:TEMP ('palpark_headless_' + $PID)
Remove-Item $w -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $w | Out-Null
$script:fail = 0
function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }

function Run([string]$name, [string[]]$more, [int]$secs = 40) {
    $log = Join-Path $w "$name.log"
    $a = @('--headless', '--frames', '5', '--log', $log, '--save-dir', (Join-Path $w 'save')) + $more
    $p = Start-Process -FilePath $rr -ArgumentList $a -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $w "$name.err") -RedirectStandardOutput (Join-Path $w "$name.out")
    $null = $p.Handle   # keep the handle so ExitCode is readable after WaitForExit
    if (-not $p.WaitForExit($secs * 1000)) { Stop-Process -Id $p.Id -Force; Fail "$name timed out"; return @{ rc = -1; log = '' } }
    $txt = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    return @{ rc = $p.ExitCode; log = $txt }
}

$fakeDs = Join-Path $w 'game.nds'; $fakeGba = Join-Path $w 'cart.gba'
[IO.File]::WriteAllBytes($fakeDs, (New-Object byte[] 4096)); [IO.File]::WriteAllBytes($fakeGba, (New-Object byte[] 4096))
$bogusCore = Join-Path $w 'no_such_core.dll'
$base = @('--core', $bogusCore, '--rom', $fakeDs, '--system', 'nds')

# 1. offline: accepted, then the missing core stops it
$r = Run 'offline' ($base + @('--slot2-rom', $fakeGba, '--slot2-save', (Join-Path $w 'x.srm')))
if ($r.log -notmatch 'slot2: accepted') { Fail 'offline: flags not accepted' }
if ($r.log -match 'REFUSED') { Fail 'offline: refused' }
if ($r.rc -ne 1) { Fail "offline: exit $($r.rc), expected 1 (core missing)" }

# 2. every online / link mode refuses
foreach ($m in @(@('net', @('--net', '127.0.0.1:9')), @('dslink', @('--ds-link')), @('rollback', @('--rollback')), @('link', @('--link')), @('hostsave', @('--share-host-save')))) {
    $r = Run $m[0] ($base + $m[1] + @('--slot2-rom', $fakeGba))
    if ($r.rc -ne 2) { Fail "$($m[0]): exit $($r.rc), expected 2" }
    if ($r.log -notmatch 'slot2: REFUSED') { Fail "$($m[0]): no REFUSED line" }
}
$r = Run 'net_offline_words' ($base + @('--net', '127.0.0.1:9', '--slot2-rom', $fakeGba))
if ($r.log -notmatch 'offline only') { Fail 'net: refusal text should say offline only' }

# 3. malformed combinations
$r = Run 'saveonly' ($base + @('--slot2-save', (Join-Path $w 'x.srm')))
if ($r.rc -ne 2) { Fail "save without rom: exit $($r.rc), expected 2" }
$r = Run 'gbagame' @('--core', $bogusCore, '--rom', $fakeGba, '--system', 'gba', '--slot2-rom', $fakeGba)
if ($r.rc -ne 2) { Fail "slot2 on a GBA game: exit $($r.rc), expected 2" }
# no slot2 flags and --net: untouched behaviour (no slot2 lines at all)
$r = Run 'plainnet' ($base + @('--net', '127.0.0.1:9'))
if ($r.log -match 'slot2:') { Fail 'online run without slot2 flags mentions slot2' }

# 4. the real core, if available
$core = $null
foreach ($d in @((Join-Path $Root 'deps\cores'), (Join-Path $Root '..\..\RetroHub\deps\cores'), (Join-Path $Root '..\..\deps\cores'))) {
    if (Test-Path (Join-Path $d 'melondsds_libretro.dll')) { $core = (Resolve-Path (Join-Path $d 'melondsds_libretro.dll')).Path; break }
}
if (-not $core) { Write-Output 'note: melondsds core not found, skipping the subsystem checks' }
else {
    $bat = Join-Path $w 'vault\gba\k\battery'; New-Item -ItemType Directory -Force $bat | Out-Null
    $sav = Join-Path $bat 'cart.srm'
    $bytes = New-Object byte[] 32768; (New-Object Random 7).NextBytes($bytes); [IO.File]::WriteAllBytes($sav, $bytes)
    $h0 = (Get-FileHash $sav -Algorithm SHA256).Hash
    for ($i = 0; $i -lt 5; $i++) {
        $r = Run "core$i" @('--core', $core, '--rom', $fakeDs, '--system', 'nds', '--slot2-rom', $fakeGba, '--slot2-save', $sav) 90
        if ($i -eq 0) {
            if ($r.log -notmatch "slot2: core subsystem '.*Slot 1 & 2") { Fail 'core does not list the Slot 1 & 2 subsystem' }
            if ($r.log -notmatch 'slot2: using game type 0x[0-9a-f]+ \(with GBA save\)') { Fail 'with-save subsystem not picked' }
        }
        Start-Sleep -Milliseconds 1100   # backup names carry seconds
    }
    if ((Get-FileHash $sav -Algorithm SHA256).Hash -ne $h0) { Fail 'vault save changed although the DS ROM never booted' }
    $baks = @(Get-ChildItem (Join-Path $bat 'palpark_backup') -Filter 'cart.srm.*.bak' -ErrorAction SilentlyContinue)
    if ($baks.Count -ne 3) { Fail "expected 3 backups kept, found $($baks.Count)" }
    foreach ($b in $baks) { if ((Get-FileHash $b.FullName -Algorithm SHA256).Hash -ne $h0) { Fail "backup $($b.Name) differs from the save" } }
    $r = Run 'nosave' @('--core', $core, '--rom', $fakeDs, '--system', 'nds', '--slot2-rom', $fakeGba, '--slot2-save', (Join-Path $w 'missing.srm')) 90
    if ($r.log -notmatch 'using game type 0x[0-9a-f]+ \(without GBA save\)') { Fail 'no-save subsystem not picked' }
}

Remove-Item $w -Recurse -Force -ErrorAction SilentlyContinue
if ($script:fail) { Write-Output "palpark_headless: $($script:fail) failure(s)"; exit 1 }
Write-Output 'palpark_headless: all passed'
exit 0
