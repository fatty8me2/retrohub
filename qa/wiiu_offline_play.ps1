# Wii U single-player through RetroHub's REAL Play path against a user-style (non-portable, %APPDATA%\Cemu) Cemu, all in scratch:
# a copy of Cemu.exe (renamed CemuQA.exe so a Cemu the user is playing in is not mistaken for ours), a scratch %APPDATA% seeded with COPIES of
# the user's settings.xml / controller profiles / shader cache / save, RetroHub's own binaries and data dir. RetroHub runs on a private
# invisible desktop, muted, no network (RETROHUB_AUTOTEST=play). The user's real Cemu folders are only ever READ (copied from).
# Checks: graphic-pack + fullscreen + CPU-mode changes reach the user-style settings while the game runs and are put back afterwards
# (settings.xml.retrohub-backup stays), the window renders (PrintWindow / Cemu F12 screenshot taken on the hidden desktop), the pad exit combo closes
# Cemu gracefully, and the save folders land in the vault.
# usage: wiiu_offline_play.ps1 -Build <bin\Release> -RomDir <folder with the .wua or .7z> -Game <room id> -Work <scratch dir> [-RealCemu <exe dir>]
#        [-RealAppData <...\Cemu>] [-Fullscreen true|false] [-GraphicPacks true|false] [-CpuMode dual-rec] [-ComboAfter 120] [-Shots "60,100"] [-Timeout 900]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$RomDir, [Parameter(Mandatory)][string]$Game, [Parameter(Mandatory)][string]$Work,
  [string]$RealCemu = "C:\Users\fatty\Desktop\Everything\Cemu_2.6", [string]$RealAppData = "$env:APPDATA\Cemu",
  [string]$Fullscreen = "false", [string]$GraphicPacks = "false", [string]$CpuMode = "dual-rec",
  [int]$ComboAfter = 120, [string]$Shots = "60,100", [int]$Timeout = 900, [string]$Desktop = "rhwiiu_off", [string]$ExtraEnv = ""
)
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class WDesk {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static int Start(string cmd, string cwd, string desk) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
    return pi.pid;
  }
}
"@
$fail = 0
function Pass($m) { Write-Host "PASS: $m" }
function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }
function Fv($xml) { if ($xml -match '<fullscreen>(\w+)</fullscreen>') { $Matches[1] } else { "" } }

if (Test-Path $Work) { Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }; Start-Sleep 2; Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$d = "$Work\hub"; $cemu = "$Work\cemu"; $ad = "$Work\appdata"; $cd = "$ad\Cemu"
New-Item -ItemType Directory -Force "$d\userdata", $cemu, $cd | Out-Null
Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
# scratch Cemu: exe + resources + shipped game profiles (no portable folder, no settings.xml beside it => %APPDATA%\Cemu = our scratch)
Copy-Item "$RealCemu\Cemu.exe" "$cemu\CemuQA.exe"
robocopy "$RealCemu\resources" "$cemu\resources" /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy "$RealCemu\gameProfiles" "$cemu\gameProfiles" /E /NFL /NDL /NJH /NJS /NP | Out-Null
# scratch %APPDATA%\Cemu: copies of the user's files, with a graphic pack entry added so the strip/restore is visible
$sx = Get-Content "$RealAppData\settings.xml" -Raw
$entry = '<GraphicPack><Entry filename="graphicPacks/retrohub_qa/rules.txt"><Preset><category>QA</category><preset>on</preset></Preset></Entry></GraphicPack>'
if ($sx -match '<GraphicPack/>') { $sx = $sx -replace '<GraphicPack/>', $entry } else { $sx = $sx -replace '</content>', "$entry</content>" }
$sx = $sx -replace '<fullscreen>\w+</fullscreen>', '<fullscreen>true</fullscreen>'
Set-Content "$cd\settings.xml" $sx -Encoding utf8
$orig = Get-Content "$cd\settings.xml" -Raw
foreach ($n in "controllerProfiles", "shaderCache") { if (Test-Path "$RealAppData\$n") { robocopy "$RealAppData\$n" "$cd\$n" /E /NFL /NDL /NJH /NJS /NP | Out-Null } }
if (Test-Path "$RealAppData\mlc01\usr\save") { robocopy "$RealAppData\mlc01\usr\save" "$cd\mlc01\usr\save" /E /NFL /NDL /NJH /NJS /NP | Out-Null }
# per-game settings (system level for Wii U): window mode, packs, CPU mode
$v = ($d -replace '\\', '/')
New-Item -ItemType Directory -Force "$d\vault\_settings" | Out-Null
Set-Content "$d\vault\_settings\game_settings.json" ('{"version":"1","systems":{"wiiu":{"fullscreen":"' + $Fullscreen + '","wiiu.graphic_packs":"' + $GraphicPacks + '","wiiu.cpu_mode":"' + $CpuMode + '"}},"games":{}}') -Encoding ascii
Set-Content "$d\userdata\hub.json" ('{"name":"qa-Wiiu","rom_folders":["' + ($RomDir -replace '\\', '/') + '"],"server":"ws://127.0.0.1:9/","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_enabled":false,"cemu_exe":"' + (($cemu -replace '\\', '/') + '/CemuQA.exe') + '"}') -Encoding ascii

$env:RETROHUB_AUTOTEST = "play"; $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_NAME = "qa-Wiiu"
$env:RETROHUB_AUTOTEST_COMBO_AFTER = "$ComboAfter"
$env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_NO_UPDATE_CHECK = "1"; $env:RETROHUB_QA = "1"
$env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"; $env:HUBNET_LOOPBACK = "1"
$env:RETROHUB_AUTOTEST_PADSWAP_AFTER = "$($ComboAfter - 30)"
$realAppdataEnv = $env:APPDATA
$env:APPDATA = $ad    # RetroHub and the Cemu it starts both inherit the scratch %APPDATA%
[WDesk]::Desk($Desktop)
$log = "$d\userdata\autotest.log"
$hubPid = [WDesk]::Start("`"$d\RetroHub.exe`"", $d, $Desktop)
$env:APPDATA = $realAppdataEnv
Write-Host "RetroHub pid $hubPid on hidden desktop '$Desktop' (APPDATA scratch: $ad)"
$t0 = Get-Date; $emuPid = 0; $bootAt = $null; $shotDone = @{}; $midChecked = $false
$mid = @{}
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
  Start-Sleep 2
  $txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
  if ($txt -match "error:|play: NOT started") { break }
  if (-not (Get-Process -Id $hubPid -ErrorAction SilentlyContinue)) { Write-Host "RetroHub exited"; break }
  if (-not $emuPid) {
    $p = Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "CemuQA.exe" -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
    if ($p) { $emuPid = $p.ProcessId; $bootAt = Get-Date; Write-Host "emulator pid $emuPid started by RetroHub: $($p.CommandLine)"; $mid.cmd = $p.CommandLine }
  }
  if ($emuPid) {
    $age = ((Get-Date) - $bootAt).TotalSeconds
    if ($age -gt 8 -and -not $midChecked) {   # while the game runs: what RetroHub put into the user-style Cemu
      $midChecked = $true
      $mid.settings = Get-Content "$cd\settings.xml" -Raw
      $mid.state = Test-Path "$cd\retrohub-cemu-state.txt"
      $mid.profile = @(Get-ChildItem "$cd\gameProfiles" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + ": " + ((Get-Content $_.FullName -Raw) -replace "`r?`n", " / ") })
    }
    foreach ($s in ($Shots -split "," | ForEach-Object { [int]$_ })) {
      if ($age -ge $s -and -not $shotDone[$s] -and (Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) {
        $shotDone[$s] = $true
        $png = "$Work\shot_${s}s.png"
        $cp = [WDesk]::Start("powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\wiiu_capture.ps1`" -TargetPid $emuPid -Out `"$png`" $(if ($env:WIIU_QA_F12) { "-F12" }) -Log `"$Work\capture_${s}s.txt`"", $Work, $Desktop)
        Write-Host "capture helper pid $cp on the hidden desktop -> $png"
      }
    }
  }
  if ($txt -match "play: emulator process ended") { break }
}
$txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
Write-Host "---- autotest.log (without the library dump) ----"
($txt -split "`n") | Where-Object { $_ -notmatch "\] game " } | ForEach-Object { Write-Host $_ }
Write-Host "---- game lines (room ids) ----"
($txt -split "`n") | Where-Object { $_ -match "\] game " } | ForEach-Object { Write-Host $_ }
if ($txt -match "play: emulator started") { Pass "emulator started through play_entry" } else { Fail "emulator was not started" }
if ($txt -match "virtual exit combo") { Pass "exit combo reached pad_exit_watch" } else { Fail "exit combo never fired" }
if ($txt -match "play: emulator process ended") { Pass "Cemu ended after the combo (graceful close)" } else { Fail "Cemu still running at timeout" }
if ($txt -match "end_session: ([^\r\n]*)") { Pass "end_session ran: $($Matches[1])" } else { Fail "end_session did not run" }
if ($txt -match "toggle pad screen hotkey") { Pass "GamePad-screen combo fired (Ctrl+Tab sent)" } else { Write-Host "NOTE: pad-screen toggle did not fire" }
if ($emuPid -and (Get-Process -Id $emuPid -ErrorAction SilentlyContinue)) { Fail "emulator pid $emuPid still alive"; Stop-Process -Id $emuPid -Force }
Write-Host "---- while the game ran ----"
if ($mid.cmd) {
  if ($mid.cmd -match ' -f\b') { Write-Host "  command line has -f" } else { Write-Host "  command line has no -f" }
  if (($Fullscreen -eq "true") -eq ($mid.cmd -match ' -f\b')) { Pass "-f agrees with the Fullscreen setting ($Fullscreen)" } else { Fail "-f disagrees with the Fullscreen setting" }
  if ((Fv $mid.settings) -eq $Fullscreen) { Pass "settings.xml fullscreen=$Fullscreen while running" } else { Fail "settings.xml fullscreen was '$(Fv $mid.settings)' while running" }
  if ($GraphicPacks -eq "false") { if ($mid.settings -match '<GraphicPack/>' -and $mid.settings -notmatch 'retrohub_qa') { Pass "graphic packs off: <GraphicPack/> while running" } else { Fail "graphic pack list not stripped while running" } }
  if ($mid.state) { Pass "retrohub-cemu-state.txt present while running" } else { Fail "no state file while running" }
  $mid.profile | ForEach-Object { Write-Host "  profile: $_" }
  if ($CpuMode -eq "game") { if (-not (($mid.profile -join " ") -match "cpuMode")) { Pass "no profile written (CPU mode = Cemu default)" } else { Fail "profile written for the default CPU mode" } } elseif (($mid.profile -join " ") -match "cpuMode") { Pass "game profile carried cpuMode while running" } else { Fail "no cpuMode in a gameProfiles ini while running" }
}
Write-Host "---- after exit ----"
$now = Get-Content "$cd\settings.xml" -Raw
Write-Host "  fullscreen now: $(Fv $now), GraphicPack entry back: $($now -match 'retrohub_qa')"
if ((Fv $now) -eq "true") { Pass "fullscreen put back to the user's value" } else { Fail "fullscreen not restored" }
if ($now -match 'graphicPacks/retrohub_qa/rules.txt') { Pass "user's graphic pack selection restored" } else { Fail "graphic pack selection lost" }
if (Test-Path "$cd\settings.xml.retrohub-backup") { Pass "settings.xml.retrohub-backup kept"; if ((Get-Content "$cd\settings.xml.retrohub-backup" -Raw) -eq $orig) { Pass "backup is byte-identical to the original" } else { Fail "backup differs from the original" } } else { Fail "no settings backup" }
if (-not (Test-Path "$cd\retrohub-cemu-state.txt")) { Pass "state file removed" } else { Fail "state file left behind" }
if (-not (Test-Path "$cd\gameProfiles\*.retrohub-*")) { Pass "no profile leftovers" } else { Fail "game profile leftovers" }
Write-Host "---- screenshots ----"
Get-ChildItem "$Work\shot_*.png", "$Work\capture_*.txt", "$cd\screenshots\*" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.FullName) $($_.Length)"; if ($_.Extension -eq ".txt") { Get-Content $_.FullName | ForEach-Object { Write-Host "      $_" } } }
Write-Host "---- vault ----"
$vault = "$d\vault"
$vf = @(Get-ChildItem $vault -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "battery" })
$vf | Select-Object -First 25 | ForEach-Object { Write-Host ("  " + $_.FullName.Substring($vault.Length) + "  " + $_.Length) }
if ($vf.Count -gt 0) { Pass "vault holds $($vf.Count) battery file(s)" } else { Fail "no battery files in the vault" }
foreach ($p in Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) }) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }   # our own scratch copies only
Write-Host "RESULT: $(if ($fail) { "FAIL ($fail)" } else { 'PASS' })  work: $Work"
exit $fail
