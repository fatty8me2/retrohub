# Remote play: guest pad -> host DSU server -> (a cemuhook client = dsu_probe), guest View+Y menu, clean guest leave, direct-UDP probe.
# Everything is a windowless console child (CreateNoWindow), a LOCAL worker is used for signalling only. Never production.
#   node qa\online_local.mjs 12210 powershell -NoProfile -File qa\rplay_dsu_loopback.ps1 -Build build\bin\Release
# Checks (prints PASS/FAIL lines, exit 1 on any FAIL):
#   1 probe: host + guest `streamrun --probe` find a direct path (result file ok, rtt >= 0)
#   2 guest scripted pad reaches the DSU server (dsu_probe sees slot 0 connected + button/stick changes)
#   3 guest View+Y menu (--test-menu) opens, toggles stats + fullscreen, Leave ends the guest, host logs guest gone, DSU slot 0 disconnects, host keeps running
#   4 host status file has the guest with an rtt, host quits on the quit event (clean exit)
param([Parameter(Mandatory)][string]$Build, [int]$DsuPort = 26790, [string]$Out = "")
. "$PSScriptRoot\_guard.ps1"
$Server = $env:RETROHUB_TEST_SERVER
if (-not $Server) { Write-Error "RETROHUB_TEST_SERVER not set (use qa\online_local.mjs)"; exit 2 }
Assert-LocalServer $Server
$B = (Resolve-Path $Build).Path
$exe = Join-Path $B "streamrun.exe"; $probeExe = Join-Path $B "dsu_probe.exe"
if (-not $Out) { $Out = Join-Path $env:TEMP ("rplay_dsu_" + [guid]::NewGuid().ToString("N").Substring(0, 6)) }
New-Item -ItemType Directory -Force $Out | Out-Null
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray()
$code = -join (1..6 | ForEach-Object { $alpha | Get-Random })
$fail = 0
function Check($ok, $what) { if ($ok) { "PASS  $what" } else { "FAIL  $what"; $script:fail++ } }
function Start-Child($exePath, $argl, $name) {   # cmd.exe redirects stderr/stdout straight into the log file (no event pumping needed)
  $log = Join-Path $Out "$name.log"; Remove-Item $log -ErrorAction SilentlyContinue
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = "cmd.exe"; $psi.Arguments = "/c `"`"$exePath`" $argl > `"$log`" 2>&1`""; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
  $p = New-Object System.Diagnostics.Process; $p.StartInfo = $psi
  $null = $p.Start()
  return $p
}
function LogOf($n) { $f = Join-Path $Out "$n.log"; if (Test-Path $f) { Get-Content $f -Raw } else { "" } }
$env:SSB64_MUTE = "1"; $env:HUBNET_FORCE_RELAY = "0"; $env:SDL_AUDIODRIVER = "dummy"

# ---- 1. probe ----
$pres = Join-Path $Out "probe.json"
$ph = Start-Child $exe "--probe --host --net $Server --room $code-101 --slot 0 --pcs 2 --probe-ms 9000 --stats-file `"$pres`"" "probe_host"
Start-Sleep -Milliseconds 600
$pg = Start-Child $exe "--probe --guest --net $Server --room $code-101 --slot 1 --pcs 2 --probe-ms 9000" "probe_guest"
$null = $ph.WaitForExit(20000); $null = $pg.WaitForExit(15000)
$pj = if (Test-Path $pres) { Get-Content $pres -Raw } else { "" }
Check ($ph.HasExited -and $ph.ExitCode -eq 0 -and $pj -match '"ok":true' -and $pj -match '"1":\d+') "probe: direct path found ($($pj.Trim()))"
foreach ($p in @($ph, $pg)) { if (-not $p.HasExited) { $p.Kill() } }
# a probe nobody joins must fail (exit 3, slot -1) without ever touching the relay
$pres2 = Join-Path $Out "probe2.json"
$ph2 = Start-Child $exe "--probe --host --net $Server --room $code-102 --slot 0 --pcs 2 --probe-ms 4000 --stats-file `"$pres2`"" "probe_none"
$null = $ph2.WaitForExit(15000)
$pj2 = if (Test-Path $pres2) { Get-Content $pres2 -Raw } else { "" }
Check ($ph2.HasExited -and $ph2.ExitCode -eq 3 -and $pj2 -match '"1":-1') "probe: no guest -> exit 3, slot 1 unreachable ($($pj2.Trim()))"

# ---- 2/3/4. stream session ----
$status = Join-Path $Out "status.json"; Remove-Item $status -ErrorAction SilentlyContinue
$room = "$code-1"
$h = Start-Child $exe "--host --test-pattern --net $Server --room $room --slot 0 --pcs 2 --dsu-port $DsuPort --status-file `"$status`"" "host"
Start-Sleep -Milliseconds 800
$dp = Start-Child $probeExe "26 $DsuPort" "dsu_probe"
$g = Start-Child $exe "--guest --headless --test-input --test-menu --net $Server --room $room --slot 1" "guest"
Start-Sleep -Seconds 5   # (the guest's scripted menu leaves at ~6.6 s; this window is "streaming")
$sj = if (Test-Path $status) { Get-Content $status -Raw } else { "" }
$null = $g.WaitForExit(40000)
Start-Sleep -Seconds 4   # lets the async log event handlers flush
$hl = LogOf "host"; $gl = LogOf "guest"
Check ($gl -match "menu opened") "guest menu opened (View+Y chord)"
Check ($gl -match "menu stats off" -or $gl -match "menu stats on") "guest menu: stats toggled"
Check ($gl -match "menu fullscreen toggled") "guest menu: fullscreen toggled"
Check ($gl -match "menu leave") "guest menu: Leave selected"
Check ($g.HasExited -and $g.ExitCode -eq 0) "guest streamrun exited cleanly after Leave (exit $($g.ExitCode))"
Check ($gl -match "sent bye") "guest sent bye before exiting"
Check ($hl -match "guest slot 1 left") "host saw the bye at once ('guest slot 1 left')"
Check (-not ($hl -match "timed out")) "host did not need the 4 s timeout"
Check (-not $h.HasExited) "host streamrun keeps running after the guest left"
$null = $dp.WaitForExit(30000)
$dl = LogOf "dsu_probe"
Check ($dl -match "slot=0 connected=1") "DSU slot 0 (guest 1) became connected"
Check (($dl -split "`n" | Where-Object { $_ -match "slot=0 connected=1 b1=0x(?!00)|slot=0 connected=1 b1=0x00 b2=0x(?!00)" }).Count -gt 0) "guest scripted pad buttons reached the DSU server"
Check ($dl -match "slot=0 connected=0") "DSU slot 0 reported disconnected after the guest left"
Check ($dl -match "summary slot=0 .*ever_connected=1 disconnected_after=1") "dsu_probe summary: slot 0 connected, then disconnected"
Check ($sj -match '"slot":1,"rtt_ms":\d+' -and $sj -match '"dsu_clients":[1-9]') "host status file while streaming: guest slot 1 with an rtt, a DSU client attached ($($sj.Trim()))"
# the host leaves on the quit event (what the hub sends instead of TerminateProcess)
$hostPid = (Get-CimInstance Win32_Process -Filter "ParentProcessId=$($h.Id)" | Where-Object { $_.Name -eq "streamrun.exe" } | Select-Object -First 1).ProcessId
$ev = [System.Threading.EventWaitHandle]::OpenExisting("Local\RetroHubStreamrunQuit_$hostPid")
$null = $ev.Set(); $ev.Dispose()
$null = $h.WaitForExit(10000)
Check ($h.HasExited -and $h.ExitCode -eq 0) "host quits cleanly on the quit event (exit $($h.ExitCode))"
"--- host.log"; LogOf "host" | Select-Object -Last 1 | Out-Null; (LogOf "host") -split "`n" | Select-Object -Last 14
"--- guest.log"; (LogOf "guest") -split "`n" | Select-Object -Last 16
"--- dsu_probe.log"; $dl
foreach ($p in @($h, $g, $dp)) { if ($p -and -not $p.HasExited) { Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }; $p.Kill() } }
if ($fail) { "RESULT: $fail FAILED"; exit 1 } else { "RESULT: all passed" }
