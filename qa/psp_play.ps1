# PSP (PPSSPP) single-player through RetroHub's REAL Play path, adapted from ext_play.ps1: RETROHUB_AUTOTEST=play presses the Play button's code
# (play_entry -> launch_external -> prepare_ppsspp -> begin_session -> PPSSPP -> end_session) on a private invisible desktop, muted, no network.
# The leave-game combo (View+Start) is simulated by RETROHUB_AUTOTEST_COMBO_AFTER=<s> (the real ExitCombo hold timer + request_game_close -> WM_CLOSE).
# The "game" is the synthetic image written by `hub_psp_test --fixture <file.iso>` (a valid PSP_GAME\PARAM.SFO and nothing else, no commercial data):
# PPSSPP starts and sits on its "can't boot" screen, which proves launch, the window close and the SAVEDATA round trip.
# -EmuDir = a scratch folder holding the UNZIPPED official PPSSPP (PPSSPPWindows64.exe at its root). It is copied into the test instance, never modified.
# usage: psp_play.ps1 -Build <bin\Release> -EmuDir <scratch PPSSPP dir> -RomDir <dir with the .iso> -Game psp:ulus10041 -Work <scratch> [-ComboAfter 25] [-Timeout 180]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$EmuDir, [Parameter(Mandatory)][string]$RomDir,
  [Parameter(Mandatory)][string]$Game, [Parameter(Mandatory)][string]$Work,
  [int]$ComboAfter = 25, [int]$Timeout = 180, [string]$Desktop = "rhpsp_play"
)
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class PspDesk {
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
if (Test-Path $d) { Remove-Item -Recurse -Force $d }
New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
$v = ($d -replace '\\', '/')
$pp = "$d\userdata\emulators\ppsspp"
robocopy $EmuDir $pp /E /NFL /NDL /NJH /NJS /NP | Out-Null
Set-Content "$pp\retrohub-install.txt" "autotest" -Encoding ascii
Remove-Item "$pp\installed.txt" -ErrorAction SilentlyContinue
# a save the emulator already holds: end_session must copy it into the vault (SAVEDATA\<ID>*)
New-Item -ItemType Directory -Force "$pp\memstick\PSP\SAVEDATA\ULUS10041DATA00" | Out-Null
Set-Content "$pp\memstick\PSP\SAVEDATA\ULUS10041DATA00\DATA.BIN" "psp-qa-save" -Encoding ascii
Set-Content "$d\userdata\hub.json" ('{"name":"qa-Ext","rom_folders":["' + ($RomDir -replace '\\', '/') + '"],"server":"ws://127.0.0.1:9/","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}') -Encoding ascii
$env:RETROHUB_AUTOTEST = "play"; $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_NAME = "qa-Ext"
$env:RETROHUB_AUTOTEST_COMBO_AFTER = "$ComboAfter"
$env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_NO_UPDATE_CHECK = "1"; $env:RETROHUB_QA = "1"
$env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"; $env:HUBNET_LOOPBACK = "1"
[PspDesk]::Desk($Desktop)
$log = "$d\userdata\autotest.log"
$hubPid = [PspDesk]::Start("$d\RetroHub.exe", $d, $Desktop)
Write-Host "RetroHub pid $hubPid on hidden desktop '$Desktop' (combo after ${ComboAfter}s of play)"
$t0 = Get-Date; $emuPid = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
  Start-Sleep 2
  $txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
  if ($txt -match "error:|play: NOT started") { break }
  if (-not (Get-Process -Id $hubPid -ErrorAction SilentlyContinue)) { Write-Host "RetroHub exited"; break }
  if (-not $emuPid) {
    $p = Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "PPSSPPWindows64.exe" -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
    if ($p) { $emuPid = $p.ProcessId; Write-Host "PPSSPP pid $emuPid ($($p.ExecutablePath)) started by RetroHub" }
  }
  if ($txt -match "play: emulator process ended") { break }
}
$txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
Write-Host "---- autotest.log (without the library dump) ----"
($txt -split "`n") | Where-Object { $_ -notmatch "\] game " } | ForEach-Object { Write-Host $_ }
if ($txt -match "play: emulator started") { Pass "PPSSPP started through play_entry" } else { Fail "PPSSPP was not started" }
if ($txt -match "virtual exit combo") { Pass "exit combo reached pad_exit_watch" } else { Fail "exit combo never fired" }
if ($txt -match "play: emulator process ended") { Pass "PPSSPP ended after the combo (graceful WM_CLOSE; RetroHub never kills it)" } else { Fail "PPSSPP still running at timeout" }
if ($txt -match "end_session: ([^\r\n]*)") { Pass "end_session ran: $($Matches[1])" } else { Fail "end_session did not run" }
if ($emuPid -and (Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) { Fail "PPSSPP pid $emuPid still alive"; Stop-Process -Id $emuPid -Force }
$vault = "$d\vault"
$vs = @(Get-ChildItem "$vault\psp" -Recurse -File -Filter DATA.BIN -ErrorAction SilentlyContinue)
if ($vs.Count -gt 0 -and (Get-Content $vs[0].FullName -Raw) -match "psp-qa-save") { Pass "SAVEDATA reached the vault: $($vs[0].FullName.Substring($vault.Length))" } else { Fail "SAVEDATA\ULUS10041DATA00\DATA.BIN is not in the vault" }
Write-Host "---- PPSSPP config readback ----"
foreach ($f in "ppsspp.ini", "controls.ini") {
  $p1 = "$pp\memstick\PSP\SYSTEM\$f"
  if (Test-Path $p1) { Pass "$f present"; Get-Content $p1 | Select-Object -First 12 | ForEach-Object { Write-Host "    $_" } } else { Fail "$f missing" }
}
if (Test-Path "$pp\installed.txt") { Fail "installed.txt appeared (the data would go to Documents)" }
foreach ($p in Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($d, [StringComparison]::OrdinalIgnoreCase) }) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }   # our own RetroHub copy only
Write-Host "RESULT: $(if ($fail) { "FAIL ($fail)" } else { 'PASS' })  work: $Work"
exit $fail
