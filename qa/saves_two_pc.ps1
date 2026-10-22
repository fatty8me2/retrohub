# Two-PC save simulation for RetroHub (headless only: retrorun --headless, qa_saves_tool).
# Rerunnable: rebuilds qa\_work from scratch each time.
#   powershell -ExecutionPolicy Bypass -File qa\saves_two_pc.ps1
# Needs build\bin\Release\{retrorun.exe,qa_saves_tool.exe} and deps\cores\mgba_libretro.dll.

$ErrorActionPreference = "Stop"
$env:SSB64_MUTE = "1"

$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "build\bin\Release"
$Rr = Join-Path $Bin "retrorun.exe"
$Q = Join-Path $Bin "qa_saves_tool.exe"
$Core = Join-Path $Root "deps\cores\mgba_libretro.dll"
$W = Join-Path $PSScriptRoot "_work"

$VA = Join-Path $W "PC_A_data\saves"
$VB = Join-Path $W "PC_B_data\saves"
$Sync = Join-Path $W "SYNC"
$RomA = Join-Path $W "PC_A_roms\sram.gba"
$RomB = Join-Path $W "PC_B_roms\deep\folder\sram.gba"
$RomZip = Join-Path $W "PC_B_roms\sram.zip"
$RomRenamed = Join-Path $W "PC_B_roms\Other Name (USA).gba"

$results = New-Object System.Collections.ArrayList
function Add-Result($name, $ok, $detail) {
    [void]$results.Add([pscustomobject]@{ Scenario = $name; Result = $(if ($ok) { "PASS" } else { "FAIL" }); Detail = $detail })
}

function Get-Sha($path) {
    if (-not (Test-Path -LiteralPath $path)) { return "MISSING" }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
}

function Parse-Kv($text, $key) {
    $m = [regex]::Match($text, "(?m)^$key=(.*)$")
    if ($m.Success) { return $m.Groups[1].Value.Trim() } else { return "" }
}

function Get-Key($rom) {
    $o = (& $Q key $rom 2>&1 | Out-String)
    return [pscustomobject]@{ Key = (Parse-Kv $o "key"); Path = (Parse-Kv $o "path"); Raw = $o }
}

function Get-Dirs($vault, $rom) {
    $o = (& $Q dirs $vault $rom 2>&1 | Out-String)
    return [pscustomobject]@{ Battery = (Parse-Kv $o "battery").TrimEnd('\'); States = (Parse-Kv $o "states").TrimEnd('\') }
}

function Invoke-Sync($vault, [switch]$States) {
    $args2 = @("sync", $vault, $Sync)
    if ($States) { $args2 += "--states" }
    $o = (& $Q @args2 2>&1 | Out-String)
    $m = [regex]::Match($o, "pushed=(\d+) pulled=(\d+) conflicts=(\d+) ok=(\d)")
    if (-not $m.Success) { return [pscustomobject]@{ Pushed = -1; Pulled = -1; Conflicts = -1; Ok = 0; Raw = $o } }
    return [pscustomobject]@{ Pushed = [int]$m.Groups[1].Value; Pulled = [int]$m.Groups[2].Value; Conflicts = [int]$m.Groups[3].Value; Ok = [int]$m.Groups[4].Value; Raw = $o }
}

function Invoke-Rr($rom, $dirs, [string[]]$extra) {
    $args2 = @("--core", $Core, "--rom", $rom, "--system", "gba", "--save-dir", $dirs.Battery, "--state-dir", $dirs.States, "--headless") + $extra
    $o = (& $Rr @args2 2>&1 | Out-String)
    $m = [regex]::Match($o, "final: frame (\d+) state-hash ([0-9a-f]+)")
    return [pscustomobject]@{ Hash = $(if ($m.Success) { $m.Groups[2].Value } else { "" }); Frame = $(if ($m.Success) { $m.Groups[1].Value } else { "" }); Raw = $o }
}

function Set-Marker($path, $tag) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($tag)
    $all = [IO.File]::ReadAllBytes($path)
    [Array]::Copy($bytes, $all, $bytes.Length)
    [IO.File]::WriteAllBytes($path, $all)
}

# ---------------------------------------------------------------- fixtures
if (Test-Path $W) { Remove-Item -Recurse -Force $W }
foreach ($d in @("PC_A_data\saves", "PC_B_data\saves", "SYNC", "PC_A_roms", "PC_B_roms\deep\folder")) {
    New-Item -ItemType Directory -Force (Join-Path $W $d) | Out-Null
}
# The stock test ROM is 2 KB, below the library's GBA minimum (64 KB): pad with 0xFF to 128 KB (header unchanged).
$src = [IO.File]::ReadAllBytes((Join-Path $Root "deps\testroms\sram.gba"))
$pad = New-Object byte[] 131072
for ($i = 0; $i -lt $pad.Length; $i++) { $pad[$i] = 0xFF }
[Array]::Copy($src, $pad, $src.Length)
[IO.File]::WriteAllBytes($RomA, $pad)
[IO.File]::WriteAllBytes($RomB, $pad)
Compress-Archive -Path $RomA -DestinationPath $RomZip -Force

# ---------------------------------------------------------------- 1. keys
$kA = Get-Key $RomA
$kB = Get-Key $RomB
$kZ = Get-Key $RomZip
Add-Result "1a key: loose ROM on PC A == folder copy on PC B" ($kA.Key -ne "" -and $kA.Key -eq $kB.Key) "$($kA.Key) vs $($kB.Key)"
Add-Result "1b key: zip of the same ROM == loose ROM" ($kZ.Key -eq $kA.Key) "$($kZ.Key)"
$romZipRun = $kZ.Path   # hub launches retrorun with the extracted rom-cache copy, not the zip

$dA = Get-Dirs $VA $RomA
$dB = Get-Dirs $VB $RomB
Add-Result "1c vault dirs follow <vault>\gba\<key>\battery|states" ($dA.Battery -like "*\gba\$($kA.Key -replace ':','_')\battery" -and $dA.States -like "*\states") "A: $($dA.Battery)"

# ---------------------------------------------------------------- 2. PC A plays, saves, syncs up
$runA = Invoke-Rr $RomA $dA @("--frames", "600")
$hashSave = $runA.Hash
Add-Result "2a PC A: 600 frames writes SRAM + auto.state" ((Test-Path "$($dA.Battery)\sram.srm") -and (Test-Path "$($dA.States)\auto.state") -and ($hashSave -ne "")) "hash $hashSave"
$shaA = Get-Sha "$($dA.Battery)\sram.srm"
$syncA = Invoke-Sync $VA -States
Add-Result "2b PC A -> SYNC pushes SRAM and auto.state" ($syncA.Ok -eq 1 -and $syncA.Pushed -eq 2) "pushed=$($syncA.Pushed)"

# ---------------------------------------------------------------- 3. PC B pulls and resumes
$syncB = Invoke-Sync $VB -States
Add-Result "3a PC B <- SYNC pulls both files" ($syncB.Ok -eq 1 -and $syncB.Pulled -eq 2) "pulled=$($syncB.Pulled)"
$shaB0 = Get-Sha "$($dB.Battery)\sram.srm"
Add-Result "3b SRAM on B is byte-identical to A" ($shaB0 -eq $shaA -and $shaA -ne "MISSING") "A $shaA / B $shaB0"
$rB = Invoke-Rr $RomB $dB @("--resume", "--frames", "0")
Add-Result "3c B resumes auto.state (frame hash == hash saved on A)" ($rB.Hash -eq $hashSave -and $rB.Raw -match "Loaded state auto") "B $($rB.Hash) vs A $hashSave"
Add-Result "3d B loads the synced SRAM (log: save: loaded)" ($rB.Raw -match "save: loaded") ""
$rZ = Invoke-Rr $romZipRun $dB @("--resume", "--frames", "0")
Add-Result "3e zip copy on B (extracted rom-cache) resumes same state" ($rZ.Hash -eq $hashSave) "$($rZ.Hash)"

# ---------------------------------------------------------------- 4. B edits its SRAM, syncs back, A gets it
# The stock test ROM's SRAM does not change with frames, so edits are made by writing a marker
# into the save file itself (what an in-game save does), then syncing.
Set-Marker (Join-Path $dB.Battery "sram.srm") "B-EDIT-1"
$shaB1 = Get-Sha "$($dB.Battery)\sram.srm"
$syncB1 = Invoke-Sync $VB -States
Add-Result "4a B -> SYNC pushes its SRAM edit" ($syncB1.Ok -eq 1 -and $syncB1.Pushed -ge 1) "pushed=$($syncB1.Pushed)"
$syncA1 = Invoke-Sync $VA -States
$shaA1 = Get-Sha "$($dA.Battery)\sram.srm"
Add-Result "4b A pulls B's edit (SRAM byte-identical to B)" ($syncA1.Ok -eq 1 -and $syncA1.Pulled -ge 1 -and $shaA1 -eq $shaB1) "pulled=$($syncA1.Pulled)"
$rA1 = Invoke-Rr $RomA $dA @("--resume", "--frames", "0")
Add-Result "4c A still resumes from the synced state after the pull" ($rA1.Raw -match "Loaded state auto" -and $rA1.Raw -match "save: loaded") "hash $($rA1.Hash)"

# ---------------------------------------------------------------- 5. conflicting SRAM edits on both PCs
$syncS = Invoke-Sync $VA -States   # clean common manifest
Set-Marker (Join-Path $dA.Battery "sram.srm") "A-CONFLICT"
$shaAc = Get-Sha "$($dA.Battery)\sram.srm"
Start-Sleep -Seconds 2
Set-Marker (Join-Path $dB.Battery "sram.srm") "B-CONFLICT"
$shaBc = Get-Sha "$($dB.Battery)\sram.srm"
$syncAc = Invoke-Sync $VA -States
$syncBc = Invoke-Sync $VB -States
$conf = @(Get-ChildItem -Recurse -File (Join-Path $W "PC_A_data\saves\_conflicts"), (Join-Path $W "PC_B_data\saves\_conflicts"), (Join-Path $W "SYNC\_conflicts") -ErrorAction SilentlyContinue)
$loserHit = @($conf | Where-Object { $_.Name -like "sram.srm.*" } | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }) -contains $shaAc
Add-Result "5a conflict: both sides changed since last sync -> conflict reported" ($syncBc.Conflicts -ge 1) "conflicts=$($syncBc.Conflicts); A pushed $($syncAc.Pushed)"
Add-Result "5b newer copy (B) wins in B vault and SYNC" ((Get-Sha "$($dB.Battery)\sram.srm") -eq $shaBc -and (Get-Sha (Join-Path $Sync "gba\gba_1337-d5dc29a9\battery\sram.srm")) -eq $shaBc) "B sha $shaBc"
Add-Result "5c loser (A's edit) kept in _conflicts with its exact bytes" $loserHit "conflict files on disk: $($conf.Count)"
$syncA2 = Invoke-Sync $VA -States
Add-Result "5d A's next sync adopts the winner (no lost data)" ((Get-Sha "$($dA.Battery)\sram.srm") -eq $shaBc) "pulled=$($syncA2.Pulled)"

# ---------------------------------------------------------------- 6. file in use during sync
# A edits SRAM and holds the file open (as a running emulator would with an open handle), then syncs.
Set-Marker (Join-Path $dA.Battery "sram.srm") "A-LOCKED"
$shaLocal = Get-Sha "$($dA.Battery)\sram.srm"
$stream = [IO.File]::Open("$($dA.Battery)\sram.srm", 'Open', 'ReadWrite', 'None')
try {
    $syncLock = Invoke-Sync $VA -States
} finally { $stream.Close() }
$skipLogged = $syncLock.Raw -match "skip"
Add-Result "6a sync with a locked, changed SRAM: ok=1, file skipped and logged" ($syncLock.Ok -eq 1 -and $skipLogged) "ok=$($syncLock.Ok) pushed=$($syncLock.Pushed) skip-logged=$skipLogged"
$syncLock2 = Invoke-Sync $VA -States
Add-Result "6b after the lock is released the next sync pushes it (retry works)" ($syncLock2.Pushed -ge 1 -and (Get-Sha (Join-Path $Sync "gba\gba_1337-d5dc29a9\battery\sram.srm")) -eq $shaLocal) "pushed=$($syncLock2.Pushed)"
# ---------------------------------------------------------------- 7. ROM renamed on PC B
# Same bytes, different file name -> different stem -> retrorun looks for "Other Name (USA).srm".
New-Item -ItemType Directory -Force (Split-Path $RomRenamed) | Out-Null
[IO.File]::WriteAllBytes($RomRenamed, $pad)
$kR = Get-Key $RomRenamed
$rR = Invoke-Rr $RomRenamed $dB @("--resume", "--frames", "0")
Add-Result "7a renamed ROM (same key) finds the synced SRAM" ($kR.Key -eq $kA.Key -and $rR.Raw -match "save: loaded") ("key same: $($kR.Key -eq $kA.Key); " + ($(if ($rR.Raw -match "save: no existing") { "log: save: no existing ... .srm" } else { "" })))

# ---------------------------------------------------------------- 8. N64 online throwaway save dir must stay out of the vault
# n64run cannot run headless, so this is a static check of the path it builds for --online sessions.
# (The sync layer mirrors everything under the vault, so a throwaway dir inside it would be pushed to every PC.)
$fe = Get-Content -Raw -LiteralPath (Join-Path $Root "src\n64run\frontend.cpp")
$onlineInVault = $fe -match 'saves\\\\online'
Add-Result "8a N64 online save dir is not under the vault (static check, n64run frontend.cpp)" (-not $onlineInVault) "saves\\online reference present: $onlineInVault"
# ---------------------------------------------------------------- 9. friends identity: the OLDEST "created" survives a first sync
# PC D = the user's original account; PC E = a new PC that made its own identity (newer file, newer "created") before syncing.
$VD = Join-Path $W "PC_D_data\saves"; $VE = Join-Path $W "PC_E_data\saves"; $SyncId = Join-Path $W "SYNC_ID"
foreach ($p in @($VD, $VE)) { New-Item -ItemType Directory -Force (Join-Path $p "_identity") | Out-Null }
$origJson = '{"user_id":"orig","secret":"s","created":1000}'
$freshJson = '{"user_id":"fresh","secret":"s","created":2000}'
[IO.File]::WriteAllText((Join-Path $VD "_identity\identity.json"), $origJson)
(Get-Item (Join-Path $VD "_identity\identity.json")).LastWriteTime = (Get-Date).AddMinutes(-30)
[IO.File]::WriteAllText((Join-Path $VE "_identity\identity.json"), $freshJson)
$null = & $Q sync $VD $SyncId 2>&1
$null = & $Q sync $VE $SyncId 2>&1
$null = & $Q sync $VD $SyncId 2>&1
$idD = (Get-Content -Raw (Join-Path $VD "_identity\identity.json")).Trim()
$idE = (Get-Content -Raw (Join-Path $VE "_identity\identity.json")).Trim()
$idS = (Get-Content -Raw (Join-Path $SyncId "_identity\identity.json")).Trim()
Add-Result "9a first sync of a new PC keeps the oldest identity everywhere" ($idD -eq $origJson -and $idE -eq $origJson -and $idS -eq $origJson) "D=$idD E=$idE sync=$idS"
$kept = @(Get-ChildItem -Recurse -File (Join-Path $VE "_conflicts\_identity") -ErrorAction SilentlyContinue)
Add-Result "9b the newer identity is kept under _conflicts" ($kept.Count -ge 1 -and ((Get-Content -Raw $kept[0].FullName).Trim() -eq $freshJson)) "files=$($kept.Count)"
# ---------------------------------------------------------------- report
$results | Format-Table -AutoSize -Wrap | Out-String -Width 250 | Write-Output
$fail = @($results | Where-Object { $_.Result -eq "FAIL" }).Count
Write-Output "PASS $(@($results | Where-Object { $_.Result -eq 'PASS' }).Count)  FAIL $fail"
if ($fail -gt 0) { exit 1 } else { exit 0 }
