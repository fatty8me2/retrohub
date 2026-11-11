# End-to-end check of the update boot marker with the REAL RetroHub.exe, headless (SDL dummy driver, no window, muted, no network).
# The update applier waits for <install>\updates\boot_ok; the hub writes it after its first presented frame (hubupdate::apply::note_boot_ok).
#   1) --boot-ok <marker> <version>: the marker appears with that version and the hub exits 0
#   2) the marker path is unwritable (a directory sits there): the hub still exits 0
#   3) no --boot-ok in headless mode: nothing is written into updates\ (QA runs stay side-effect free)
# Usage: qa\boot_ok_check.ps1          Runs from a sandbox COPY of build\bin\Release (never the build folder itself).
$ErrorActionPreference = "Stop"
$busy = Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and -not $_.Path.StartsWith('C:\Users\fatty\Desktop\SmashClassicPort\wt\') -and ($_.Path -notlike '*\Temp\*') }
if ($busy) { $busy | Select-Object Name, Path | Out-String | Write-Host; Write-Host "SKIP: a game is running (owner is gaming); try again later"; exit 3 }

$root = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root "build\bin\Release"
$sb = Join-Path $root "build\qa_bootok"
if (-not (Test-Path (Join-Path $src "RetroHub.exe"))) { Write-Host "FAIL: build RetroHub first ($src\RetroHub.exe missing)"; exit 1 }
if (Test-Path $sb) { Remove-Item $sb -Recurse -Force }
New-Item -ItemType Directory -Force $sb | Out-Null
Get-ChildItem $src -Force | Where-Object { $_.Name -ne "userdata" } | ForEach-Object { Copy-Item $_.FullName (Join-Path $sb $_.Name) -Recurse -Force }
$exe = Join-Path $sb "RetroHub.exe"
$ver = "v9.9.9"
$fails = 0
function Check($ok, $what) { if ($ok) { Write-Host "PASS $what" } else { Write-Host "FAIL $what"; $script:fails++ } }

function Run-Hub([string[]]$HubArgs, [string]$Tag) {
    $bmp = Join-Path $sb "$Tag.bmp"
    $set = @{ RETROHUB_QA = "1"; RETROHUB_NO_SHELL = "1"; SSB64_MUTE = "1"; RETRORUN_MUTE = "1"; N64RUN_MUTE = "1"; HUB_SCREENSHOT = $bmp;
              HUB_SHOT_SIZE = "1280x800"; HUB_SHOT_NOWAIT = "1"; HUB_SHOT_SECS = "2"; HUB_SHOT_FAKE_LIB = "6"; HUB_SHOT_VAULT = (Join-Path $sb "vault_$Tag") }
    foreach ($k in $set.Keys) { Set-Item "env:$k" $set[$k] }
    $p = $null
    try {
        $p = if ($HubArgs.Count) { Start-Process $exe -ArgumentList $HubArgs -PassThru -WindowStyle Hidden -WorkingDirectory $sb } else { Start-Process $exe -PassThru -WindowStyle Hidden -WorkingDirectory $sb }
        $done = $p.WaitForExit(90000)
        if (-not $done) { Stop-Process -Id $p.Id -Force; return @{ code = -999; frame = $false } }   # only the process THIS script started, by PID
        $p.WaitForExit()
        return @{ code = $p.ExitCode; frame = (Test-Path $bmp) }
    } finally { foreach ($k in $set.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue } }
}

# 1) marker requested on the command line
$m1 = Join-Path $sb "updates\boot_ok"
$r = Run-Hub @("--boot-ok", "`"$m1`"", $ver) "run1"
Check ($r.code -eq 0) "run 1: hub exits 0 (exit $($r.code))"
Check $r.frame "run 1: a frame was rendered"
$txt = if (Test-Path $m1 -PathType Leaf) { (Get-Content $m1 -Raw).Trim() } else { "<missing>" }
Check ($txt -eq $ver) "run 1: marker exists and holds '$ver' (got '$txt')"

# 2) the marker path cannot be written (a directory is in its place): the hub must not care
$m2 = Join-Path $sb "updates2\boot_ok"
New-Item -ItemType Directory -Force $m2 | Out-Null
$r = Run-Hub @("--boot-ok", "`"$m2`"", $ver) "run2"
Check ($r.code -eq 0) "run 2: unwritable marker, hub still exits 0 (exit $($r.code))"
Check ((Test-Path $m2 -PathType Container)) "run 2: the blocking directory is untouched"

# 3) headless without the argument: no side effect in updates\
Remove-Item (Join-Path $sb "updates") -Recurse -Force -ErrorAction SilentlyContinue
$r = Run-Hub @() "run3"
Check ($r.code -eq 0) "run 3: plain headless run exits 0 (exit $($r.code))"
Check (-not (Test-Path (Join-Path $sb "updates\boot_ok"))) "run 3: no updates\boot_ok written without --boot-ok"

Remove-Item $sb -Recurse -Force -ErrorAction SilentlyContinue
if ($fails) { Write-Host "RESULT: $fails check(s) FAILED"; exit 1 }
Write-Host "RESULT: all checks passed"
