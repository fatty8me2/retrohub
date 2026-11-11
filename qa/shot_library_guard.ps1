# Guard: a headless shot with fake-history hooks must NEVER modify <exe>\userdata\library.json (the player's real play history).
# Copies the build output to a temp folder, plants a library.json (with a real-looking game and a fake: one), runs one hub with HUB_SHOT_FAKE_LIB + CONTINUE + STATES + PLAYTIME,
# then asserts the bytes of library.json are unchanged. Exit 0 = pass, 1 = fail, 3 = owner gaming (refused).
param([string]$Build = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot "_gamecheck.ps1")
if (Test-OwnerGaming) { Write-Host "REFUSED: the owner is gaming"; exit 3 }
if (-not $Build) { $Build = Join-Path $root "build\bin\Release" }
$tmp = Join-Path $env:TEMP ("rh_libguard_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
& robocopy $Build $tmp /MIR /XD userdata /NFL /NDL /NJH /NJS /NP | Out-Null
$ud = Join-Path $tmp "userdata"
New-Item -ItemType Directory -Force $ud | Out-Null
$lib = Join-Path $ud "library.json"
[IO.File]::WriteAllText($lib, '{"version":1,"games":{"n64:realgame":{"last_played":1700000000,"play_seconds":123,"favorite":true},"fake:2":{"last_played":1700000000,"play_seconds":999}}}', (New-Object System.Text.UTF8Encoding $false))
$before = (Get-FileHash $lib -Algorithm SHA256).Hash
$vault = Join-Path $tmp "vault"
New-Item -ItemType Directory -Force (Join-Path $vault "_account") | Out-Null
$bmp = Join-Path $tmp "g.bmp"
$psi = New-Object System.Diagnostics.ProcessStartInfo (Join-Path $tmp "RetroHub.exe")
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WorkingDirectory = $tmp
foreach ($kv in @{ RETROHUB_NO_SHELL = "1"; RETROHUB_QA = "1"; SSB64_MUTE = "1"; HUB_SCREENSHOT = $bmp; HUB_SHOT_SIZE = "1280x720"; HUB_SHOT_SECS = "3"; HUB_SHOT_NOWAIT = "1"
                  HUB_SHOT_VAULT = $vault; HUB_SHOT_FAKE_LIB = "12"; HUB_SHOT_CONTINUE = "1"; HUB_SHOT_STATES = "1"; HUB_SHOT_PLAYTIME = "1"; HUB_SHOT_FRESH = "1" }.GetEnumerator()) { $psi.EnvironmentVariables[$kv.Key] = $kv.Value }
$p = [System.Diagnostics.Process]::Start($psi)
if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force }   # our own PID only
$p.WaitForExit()
$after = (Get-FileHash $lib -Algorithm SHA256).Hash
$shot = Test-Path $bmp
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($p.ExitCode -eq 0 -and $shot -and $before -eq $after) { Write-Host "PASS: userdata\library.json unchanged (exit 0, shot taken)"; exit 0 }
Write-Host "FAIL: exit=$($p.ExitCode) shot=$shot unchanged=$($before -eq $after)"
exit 1
