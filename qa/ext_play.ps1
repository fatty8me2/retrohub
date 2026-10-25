# 3DS (Azahar) and Wii U (Cemu) single-player through RetroHub's REAL Play path: RETROHUB_AUTOTEST=play presses the Play button's code
# (play_entry -> launch_external -> begin_session -> emulator -> end_session) on a private invisible desktop, muted, no network.
# The leave-game combo (View+Start) is simulated by RETROHUB_AUTOTEST_COMBO_AFTER=<s> (hooks pad_exit_watch's pad query, so the
# real ExitCombo hold timer + request_game_close -> WM_CLOSE path runs). Nothing here touches a user install: -EmuDir must be a
# scratch copy (Azahar: the extracted release folder; Cemu: a scratch copy that gets a `portable` folder so %APPDATA%\Cemu is never used).
# usage: ext_play.ps1 -Build <bin\Release> -Kind 3ds|wiiu -EmuDir <scratch emulator dir> -RomDir <dir with the game> -Game <room id> -Work <scratch>
#        [-ComboAfter 45] [-Timeout 300] [-Desktop rhext_play] [-SeedSaveRel <relative file under the emulator save root to drop before exit>]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][ValidateSet("3ds", "wiiu")][string]$Kind, [Parameter(Mandatory)][string]$EmuDir,
  [Parameter(Mandatory)][string]$RomDir, [Parameter(Mandatory)][string]$Game, [Parameter(Mandatory)][string]$Work,
  [int]$ComboAfter = 45, [int]$Timeout = 300, [string]$Desktop = "rhext_play", [switch]$KeepWork
)
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class ExtDesk {
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
function Pass($m) { Write-Host "PASS: $m" }
function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }
New-Item -ItemType Directory -Force $Work | Out-Null
$d = Join-Path $Work "inst0"
Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }   # leftovers of an aborted run of THIS script
Start-Sleep 2
if (Test-Path $d) { Remove-Item -Recurse -Force $d }
New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
$v = ($d -replace '\\', '/')
$cemuExe = ""
if ($Kind -eq "3ds") {
  $az = "$d\userdata\emulators\azahar"
  robocopy $EmuDir $az /E /NFL /NDL /NJH /NJS /NP | Out-Null
  Set-Content "$az\retrohub-install.txt" "autotest" -Encoding ascii
  # an empty "user" folder beside azahar.exe is what makes a copy portable; RetroHub's prepare_azahar writes qt-config.ini into it
} else {
  $cemuExe = (Join-Path $EmuDir "Cemu.exe") -replace '\\', '/'
  if (-not (Test-Path "$EmuDir\portable")) { Fail "scratch Cemu has no portable folder: refusing (it would use %APPDATA%\Cemu)"; exit 1 }
}
$cemuJson = if ($cemuExe) { ',"cemu_exe":"' + $cemuExe + '"' } else { "" }
Set-Content "$d\userdata\hub.json" ('{"name":"qa-Ext","rom_folders":["' + ($RomDir -replace '\\', '/') + '"],"server":"ws://127.0.0.1:9/","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false' + $cemuJson + '}') -Encoding ascii
$env:RETROHUB_AUTOTEST = "play"; $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_NAME = "qa-Ext"
$env:RETROHUB_AUTOTEST_COMBO_AFTER = "$ComboAfter"
$env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_NO_UPDATE_CHECK = "1"; $env:RETROHUB_QA = "1"
$env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"; $env:HUBNET_LOOPBACK = "1"
[ExtDesk]::Desk($Desktop)
$log = "$d\userdata\autotest.log"
$hubPid = [ExtDesk]::Start("$d\RetroHub.exe", $d, $Desktop)
Write-Host "RetroHub pid $hubPid on hidden desktop '$Desktop' (combo after ${ComboAfter}s of play)"
$t0 = Get-Date; $emuPid = 0; $bootSeen = $null
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
  Start-Sleep 2
  $txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
  if ($txt -match "error:|play: NOT started") { break }
  if (-not (Get-Process -Id $hubPid -ErrorAction SilentlyContinue)) { Write-Host "RetroHub exited"; break }
  if (-not $emuPid) {
    $p = Get-CimInstance Win32_Process | Where-Object { ($_.Name -eq "azahar.exe" -or $_.Name -eq "Cemu.exe") -and $_.ExecutablePath -and ($_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) -or $_.ExecutablePath.StartsWith($EmuDir, [StringComparison]::OrdinalIgnoreCase)) } | Select-Object -First 1
    if ($p) { $emuPid = $p.ProcessId; $bootSeen = Get-Date; Write-Host "emulator pid $emuPid ($($p.ExecutablePath)) started by RetroHub" }
  }
  if ($txt -match "play: emulator process ended") { break }
}
$txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
Write-Host "---- autotest.log (without the library dump) ----"
($txt -split "`n") | Where-Object { $_ -notmatch "\] game " } | ForEach-Object { Write-Host $_ }
if ($txt -match "play: emulator started") { Pass "emulator started through play_entry" } else { Fail "emulator was not started" }
if ($txt -match "virtual exit combo") { Pass "exit combo reached pad_exit_watch" } else { Fail "exit combo never fired" }
if ($txt -match "play: emulator process ended") { Pass "emulator process ended after the combo (graceful WM_CLOSE; RetroHub never kills it)" } else { Fail "emulator still running at timeout" }
if ($txt -match "end_session: ([^\r\n]*)") { Pass "end_session ran: $($Matches[1])" } else { Fail "end_session did not run" }
if ($emuPid -and (Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) { Fail "emulator pid $emuPid still alive"; Stop-Process -Id $emuPid -Force }
Write-Host "---- vault ----"
$vault = "$d\vault"
$vf = @(Get-ChildItem $vault -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "battery" })
$vf | Select-Object -First 25 | ForEach-Object { Write-Host ("  " + $_.FullName.Substring($vault.Length) + "  " + $_.Length) }
if ($vf.Count -gt 0) { Pass "vault holds $($vf.Count) battery file(s)" } else { Fail "no battery files in the vault" }
if ($Kind -eq "3ds") {
  $ini = "$d\userdata\emulators\azahar\user\config\qt-config.ini"
  Write-Host "---- qt-config.ini [Controls] / [UI] readback (after Azahar exited and rewrote it) ----"
  if (Test-Path $ini) {
    $lines = Get-Content $ini
    $lines | Select-String -Pattern '^button_(a|b|start|select)=|^circle_pad=|^confirmClose=' | ForEach-Object { Write-Host "  $($_.Line)" }
    if ($lines -match '^button_a=.*engine:sdl,api:controller,button:\d+') { Pass "controller mapping survived Azahar's own config rewrite" } else { Fail "button_a binding missing or rewritten" }
  } else { Fail "qt-config.ini missing" }
  Get-Content "$d\userdata\emulators\azahar\user\config\retrohub-pad.txt" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  marker: $_" }
} else {
  $pd = "$EmuDir\portable"
  Write-Host "---- Cemu controllerProfiles readback ----"
  Get-ChildItem "$pd\controllerProfiles" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.Name)" }
  Get-ChildItem "$pd\controllerProfiles.retrohub-backup" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  backup: $($_.Name)" }
  if (Test-Path "$pd\controllerProfiles\controller0.xml") { Pass "controller0.xml written"; Get-Content "$pd\controllerProfiles\controller0.xml" | Select-Object -First 14 | ForEach-Object { Write-Host "    $_" } } else { Fail "controller0.xml missing" }
  if (Test-Path "$pd\controllerProfiles.retrohub-backup") { Pass "backup folder created" } else { Fail "no backup folder" }
}
foreach ($p in Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($d, [StringComparison]::OrdinalIgnoreCase) }) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }   # our own RetroHub copy only
Write-Host "RESULT: $(if ($fail) { "FAIL ($fail)" } else { 'PASS' })  work: $Work"
exit $fail
