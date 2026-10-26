# Wii U remote play through RetroHub's REAL room flow: a host hub (scratch managed Cemu + the extracted/archived game) and a guest hub (no game, no Cemu)
# on ONE private invisible desktop, against a LOCAL worker (signalling only; media is direct UDP on loopback). Driven by RETROHUB_AUTOTEST.
#   node qa\online_local.mjs 12220 powershell -NoProfile -File qa\wiiu_remote_room.ps1 -Build build\bin\Release -Archive "<game>.7z" [-Scenario leave|hostquit] [-CemuSrc C:\rhq\cemu_stream]
# Scenarios
#   leave     the guest's streamrun scripts View+Y -> Leave (--test-menu) once video flows; the host must see the bye, drop the DSU pad and keep Cemu
#             running; then the host closes Cemu (QUIT_AFTER) and its streamrun ends.
#   hostquit  the guest only streams (scripted pad); the host closes Cemu while the guest is connected -> the guest's viewer ends and its hub is back in the room.
# Video: WGC/DXGI cannot capture Cemu on a hidden desktop, so the host streams the TEST PATTERN (RETROHUB_AUTOTEST_TESTPATTERN=1). A real Cemu capture needs one run on
# the interactive desktop (docs/REMOTE_PLAY.md). Muted; nothing visible; kills only the PIDs under $Work.
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Archive, [string]$CemuSrc = "C:\rhq\cemu_stream", [string]$Work = "C:\rhq\wiiu_int\e2e",
  [string]$Scenario = "leave", [int]$Seconds = 420, [string]$GameTitle = "Super Mario 3D World", [string]$Desktop = "rhwiiu_int", [int]$QuitAfter = 40
)
. "$PSScriptRoot\_guard.ps1"
$Server = $env:RETROHUB_TEST_SERVER
if (-not $Server) { Write-Error "RETROHUB_TEST_SERVER not set (use qa\online_local.mjs)"; exit 2 }
Assert-LocalServer $Server
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class RDesk {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static int Start(string exe, string cwd, string desk) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, "\"" + exe + "\"", IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
    return pi.pid;
  }
}
"@
$fail = 0
function Check($ok, $what) { if ($ok) { "PASS  $what" } else { "FAIL  $what"; $script:fail++ } }
function Txt($p) { if (Test-Path $p) { Get-Content $p -Raw } else { "" } }
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray()
$Code = -join (1..6 | ForEach-Object { $alpha | Get-Random })
if (Test-Path $Work) {   # previous run: only processes that live under $Work
  Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep 2
  Remove-Item -Recurse -Force "$Work\inst0\userdata\autotest.log", "$Work\inst1\userdata\autotest.log", "$Work\inst0\streamrun.log", "$Work\inst1\streamrun.log" -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force $Work, "$Work\games", "$Work\noroms" | Out-Null
if (-not (Get-ChildItem "$Work\games" -ErrorAction SilentlyContinue)) { Copy-Item $Archive "$Work\games\" }   # the original archive is only ever read
[RDesk]::Desk($Desktop)
$pids = @()
for ($i = 0; $i -lt 2; $i++) {
  $role = if ($i -eq 0) { "host" } else { "join" }
  $d = "$Work\inst$i"
  New-Item -ItemType Directory -Force "$d\userdata" | Out-Null
  Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
  if ($i -eq 0) {   # scratch managed Cemu: exe + resources + portable settings/mlc; the marker file is written last
    $cz = "$d\userdata\emulators\cemu"
    if (-not (Test-Path "$cz\retrohub-install.txt")) {
      New-Item -ItemType Directory -Force $cz | Out-Null
      Copy-Item "$CemuSrc\Cemu.exe" $cz
      robocopy "$CemuSrc\resources" "$cz\resources" /E /NFL /NDL /NJH /NJS /NP | Out-Null
      if (Test-Path "$CemuSrc\gameProfiles") { robocopy "$CemuSrc\gameProfiles" "$cz\gameProfiles" /E /NFL /NDL /NJH /NJS /NP | Out-Null }
      if (Test-Path "$CemuSrc\portable") { robocopy "$CemuSrc\portable" "$cz\portable" /E /NFL /NDL /NJH /NJS /NP /XF log.txt | Out-Null }
      Set-Content "$cz\retrohub-install.txt" "autotest" -Encoding ascii
    }
    Remove-Item "$cz\portable\log.txt" -ErrorAction SilentlyContinue
  }
  $v = ($d -replace '\\', '/')
  $roms = if ($i -eq 0) { "$Work\games" } else { "$Work\noroms" }
  Set-Content "$d\userdata\hub.json" ('{"name":"qa-At' + $i + '","rom_folders":["' + ($roms -replace '\\', '/') + '"],"server":"' + $Server + '","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}') -Encoding ascii
  $env:RETROHUB_AUTOTEST = $role; $env:RETROHUB_AUTOTEST_CODE = $Code; $env:RETROHUB_AUTOTEST_NAME = "qa-At$i"; $env:RETROHUB_AUTOTEST_PLAYERS = "2"
  Remove-Item Env:RETROHUB_AUTOTEST_GAME, Env:RETROHUB_AUTOTEST_LEAVE_AFTER, Env:RETROHUB_AUTOTEST_QUIT_AFTER, Env:RETROHUB_AUTOTEST_STREAMRUN_ARGS, Env:RETROHUB_AUTOTEST_TESTPATTERN -ErrorAction SilentlyContinue
  if ($i -eq 0) {
    $env:RETROHUB_AUTOTEST_GAME = "title:$GameTitle"; $env:RETROHUB_AUTOTEST_TESTPATTERN = "1"; $env:RETROHUB_NO_EXISTING_EMU = "1"
    if ($Scenario -eq "hostquit" -or $Scenario -eq "leave") { $env:RETROHUB_AUTOTEST_QUIT_AFTER = "$QuitAfter" }
  } else {
    $env:RETROHUB_AUTOTEST_STREAMRUN_ARGS = if ($Scenario -eq "leave") { "--test-input --test-menu --test-menu-at 15" } else { "--test-input" }
  }
  $env:STREAMRUN_LOG = "$d\streamrun.log"; $env:SDL_AUDIODRIVER = "dummy"
  $env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_NO_UPDATE_CHECK = "1"; $env:RETROHUB_QA = "1"
  $env:HUBNET_LOOPBACK = "1"; Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue
  $env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"
  $pids += [RDesk]::Start("$d\RetroHub.exe", $d, $Desktop)
  Write-Host "started $role inst$i pid $($pids[-1]) room $Code scenario $Scenario"
  Start-Sleep 6
}
$h = "$Work\inst0\userdata\autotest.log"; $g = "$Work\inst1\userdata\autotest.log"
$end = (Get-Date).AddSeconds($Seconds); $ok = $false
while ((Get-Date) -lt $end) {
  Start-Sleep 3
  $ht = Txt $h; $gt = Txt $g
  if ($ht -match "error:" -or $gt -match "error:") { Write-Host "autotest error"; break }
  if ($Scenario -eq "leave" -and $ht -match "game process ended") { $ok = $true; break }
  if ($Scenario -eq "hostquit" -and $ht -match "game process ended" -and $gt -match "game process ended") { $ok = $true; break }
  if (-not ($pids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })) { break }
}
Start-Sleep 5
$ht = Txt $h; $gt = Txt $g; $hs = Txt "$Work\inst0\streamrun.log"; $gs = Txt "$Work\inst1\streamrun.log"
$cz = "$Work\inst0\userdata\emulators\cemu"
Write-Host "================ host autotest.log (no library dump)"; ($ht -split "`n") | Where-Object { $_ -notmatch "\] game " } | Select-Object -Last 40
Write-Host "================ guest autotest.log"; ($gt -split "`n") | Where-Object { $_ -notmatch "\] game " } | Select-Object -Last 25
Write-Host "================ host streamrun.log"; ($hs -split "`n") | Select-Object -Last 30
Write-Host "================ guest streamrun.log"; ($gs -split "`n") | Select-Object -Last 30
"================ checks"
Check ($ht -match "launching .*Mario" -or $ht -match "launching") "host launched the remote-play game"
Check ($ht -match "rplay: probe started" -and $gt -match "rplay: probe joined") "pre-check: host started a probe and the guest joined it"
Check ($ht -match "rplay: probe OK") "pre-check: direct path found, launch followed"
Check ($ht -match "rplay: streamrun attached") "host streamrun attached to Cemu"
Check ($hs -match "DSU server on 127.0.0.1:\d+ listening") "host streamrun runs the DSU server"
$prof = Txt "$cz\portable\controllerProfiles\controller1.xml"
Check ($prof -match "DSUController") "Cemu profile controller1.xml binds player 2 to DSUController (remote play launch)"
Check ((Txt "$cz\portable\controllerProfiles\controller0.xml") -notmatch "DSUController") "player 1 (the host) is not on DSU"
Check ($hs -match "guest slot 1 joined") "guest slot 1 joined the stream"
Check ($gs -match "session ready \(direct UDP, relay disabled\)") "guest connected directly (relay disabled)"
$st = Txt "$Work\inst0\userdata\rplay_status.json"
Check ($ht -match "rplay-pads: guest pad packets seen by the host = [1-9]" -and $hs -match "pads=[1-9]\d*") "guest scripted pad packets reached the host's DSU feed (host streamrun: $([regex]::Match($hs, 'pads=\d+').Value))"
$clog = Txt "$cz\portable\log.txt"
Check ($ht -match "rplay-dsu: clients=[1-9]") "Cemu connected to the DSU server as a client (status dsu_clients >= 1)"
Check ($ht -match "rplay-status: streaming to") "host status line produced ('streaming to <guest> . Mbps . ms')"
if ($Scenario -eq "leave") {
  Check ($gs -match "menu opened" -and $gs -match "menu leave") "guest View+Y menu opened and Leave chosen"
  Check ($hs -match "guest slot 1 left" -and $hs -match "slot 1 gone \(pad disconnected\)") "host saw the guest leave at once and disconnected the pad"
  Check (-not ($hs -match "timed out")) "no 4 s timeout needed"
  Check ($ht -match "closing the game \(host\)") "host closed Cemu afterwards (host kept running after the guest left)"
}
if ($Scenario -eq "hostquit") {
  Check ($gt -match "game process ended; in room=yes") "guest viewer ended and the guest hub is back in the room"
}
Check ($ht -match "game process ended") "host Cemu ended and the host streamrun was asked to quit"
Check ($ht -match "rplay: asked pid") "hub used the quit event, not TerminateProcess"
# kill what is left, our own scratch copies only
foreach ($p in Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) }) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
foreach ($p in $pids) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }
if ($fail) { "RESULT: $fail FAILED"; exit 1 } else { "RESULT: all passed" }

