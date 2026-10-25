# GameCube cheats through RetroHub's REAL Play path (RETROHUB_AUTOTEST=play -> play_entry -> launch_external -> cheats_apply_dolphin ->
# Dolphin), on a private invisible desktop, muted, no network. For each run RetroHub switches the named database cheats on
# (RETROHUB_AUTOTEST_CHEATS="desc;desc", matched by description substring), writes User\GameSettings\<GAMEID>.ini, Dolphin boots the
# game and a screenshot of its render window is saved. Pass -Cheats "" for the control run (nothing on: the file must be clean).
# Pad input is not deliverable on a hidden desktop, so the proof reads GAME MEMORY instead: Dolphin runs with its GDB stub
# (-C Dolphin.General.GDBPort), qa\gdb_read_mem.py lets the game run, breaks in and reads the unlock flags at -Mem (Melee r2:
# 0x8045BF28/2C). With the cheat on they must equal -MemOn; with the control run (cheats "-") they must differ.
# usage: gc_cheats_play.ps1 -Build <bin\Release> -DolphinDir <deps\dolphin> -Work <scratch> -DiscDir <dir with an extracted disc>
#        -Game <room id, e.g. melee> -GameId GALE01 -Runs "name=cheat1;cheat2","name2=" [-Shots <dir>] [-Seconds 40]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$DolphinDir, [Parameter(Mandatory)][string]$Work,
  [Parameter(Mandatory)][string]$DiscDir, [Parameter(Mandatory)][string]$Game, [string]$GameId = "GALE01", [string[]]$Runs = @("on=Unlock All Characters and Stages", "off=-"),
  [string]$Shots = "", [int]$Seconds = 45, [string]$Desktop = "rhgc_cheats",
  [string]$Mem = "8045BF28:8", [string]$MemOn = "FFFFFFFFFFFFFFFF", [int]$RunSeconds = 12, [switch]$DebugLog
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class GcDesk {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenDesktop(string n, int f, bool inh, uint a);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr d, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr w, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static int Start(string exe, string cwd, string desk) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, "\"" + exe + "\"", IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
    return pi.pid;
  }
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  // Screenshot of the biggest window of a pid on the hidden desktop (PrintWindow); returns false if none.
  public static bool Shot(string desk, int pid, string file) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); IntPtr best = IntPtr.Zero; int area = 0;
    EnumDesktopWindows(d, (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p != pid) return true;
      { RECT r0; GetWindowRect(h, out r0); int a0 = (r0.r - r0.l) * (r0.b - r0.t); var cn = new System.Text.StringBuilder(256); GetWindowText(h, cn, 256); if (cn.ToString().Contains("JIT") || cn.ToString().Contains("Direct3D")) { area = int.MaxValue; best = h; } else if (a0 > area) { area = a0; best = h; } }
      EnumChildWindows(h, (c, l2) => { RECT r; GetWindowRect(c, out r); int a = (r.r - r.l) * (r.b - r.t); if (a > area) { area = a; best = c; } return true; }, IntPtr.Zero); return true; }, IntPtr.Zero);
    if (best == IntPtr.Zero) return false;
    RECT rc; GetWindowRect(best, out rc); int w = rc.r - rc.l, hh = rc.b - rc.t; if (w <= 0 || hh <= 0) return false;
    using (var bmp = new System.Drawing.Bitmap(w, hh)) { using (var g = System.Drawing.Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(best, dc, 2); g.ReleaseHdc(dc); } bmp.Save(file, System.Drawing.Imaging.ImageFormat.Png); }
    return true;
  }
  public static void Close(string desk, int pid) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF);
    EnumDesktopWindows(d, (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p == pid) PostMessage(h, 0x10, IntPtr.Zero, IntPtr.Zero); return true; }, IntPtr.Zero);
  }
  // Posts Esc to every window (top level + children) owned by one of the pids; returns a description of what it hit.
  public static string Esc(string desk, int[] pids) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); var hit = new List<string>();
    var set = new HashSet<int>(pids);
    EnumDesktopWindows(d, (h, l) => {
      int pid; GetWindowThreadProcessId(h, out pid);
      if (!set.Contains(pid)) return true;
      var c = new System.Text.StringBuilder(128); var t = new System.Text.StringBuilder(256);
      GetClassName(h, c, 128); GetWindowText(h, t, 256);
      hit.Add(pid + ":" + c + ":" + t);
      PostMessage(h, 0x1C, (IntPtr)1, IntPtr.Zero); PostMessage(h, 0x6, (IntPtr)1, IntPtr.Zero); PostMessage(h, 0x7, IntPtr.Zero, IntPtr.Zero);   // WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS: Qt only routes keys to an active window
      PostMessage(h, 0x100, (IntPtr)0x1B, (IntPtr)0x00010001); PostMessage(h, 0x101, (IntPtr)0x1B, unchecked((IntPtr)0xC0010001));
      EnumChildWindows(h, (ch, l2) => { PostMessage(ch, 0x100, (IntPtr)0x1B, (IntPtr)0x00010001); PostMessage(ch, 0x101, (IntPtr)0x1B, unchecked((IntPtr)0xC0010001)); return true; }, IntPtr.Zero);
      return true; }, IntPtr.Zero);
    return string.Join(" | ", hit);
  }
}
"@
function Sha($p) { if (Test-Path $p) { (Get-FileHash $p -Algorithm SHA1).Hash.Substring(0, 10) } else { "-" } }
$fail = 0
function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }
function Pass($m) { Write-Host "PASS: $m" }
New-Item -ItemType Directory -Force $Work | Out-Null
$d = Join-Path $Work "inst0"
Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }   # leftovers of THIS script's own run
Start-Sleep 2
if (Test-Path $d) { Remove-Item -Recurse -Force $d }
New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
New-Item -ItemType Directory -Force "$d\data" | Out-Null
Copy-Item (Join-Path $PSScriptRoot "..\data\cheats_db.bin"), (Join-Path $PSScriptRoot "..\data\cheats_featured.json") "$d\data"
robocopy $DolphinDir "$d\userdata\emulators\dolphin" /E /XD _downloads User /NFL /NDL /NJH /NJS /NP | Out-Null
Set-Content "$d\userdata\emulators\dolphin\retrohub-install.txt" "autotest" -Encoding ascii
if ($DebugLog) {   # Dolphin file log (flushed on the clean WM_CLOSE exit): shows which Gecko / AR codes it loaded
  $cfg = "$d\userdata\emulators\dolphin\User\Config"; New-Item -ItemType Directory -Force $cfg | Out-Null
  Set-Content "$cfg\Logger.ini" "[Options]`nVerbosity = 5`nWriteToFile = True`nWriteToConsole = False`nWriteToWindow = False`n[Logs]`nACTIONREPLAY = True`nCORE = True`nBOOT = True`nMASTER = True" -Encoding ascii
}
$v = ($d -replace '\\', '/')
Set-Content "$d\userdata\hub.json" ('{"name":"qa-GcCheats","rom_folders":["' + ($DiscDir -replace '\\', '/') + '"],"server":"ws://127.0.0.1:9/","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}') -Encoding ascii
$env:RETROHUB_AUTOTEST = "play"; $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_NAME = "qa-GcCheats"
$env:RETROHUB_AUTOTEST_DOLPHIN_ARGS = "-C Dolphin.Interface.ConfirmStop=False -C Dolphin.DSP.Volume=0 -C Dolphin.Core.EmulationSpeed=0"
$env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_NO_UPDATE_CHECK = "1"; $env:RETROHUB_QA = "1"
$env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"; $env:HUBNET_LOOPBACK = "1"
[GcDesk]::Desk($Desktop)
$log = "$d\userdata\autotest.log"
$gini = "$d\userdata\emulators\dolphin\User\GameSettings\$GameId.ini"
$dini = "$d\userdata\emulators\dolphin\User\Config\Dolphin.ini"
if ($Shots) { New-Item -ItemType Directory -Force $Shots | Out-Null }

foreach ($run in $Runs) {
  $name, $cheats = $run -split "=", 2
  Write-Host "=== run '$name' cheats: [$cheats]"
  $env:RETROHUB_AUTOTEST_CHEATS = $cheats   # "-" = nothing on (the QA hook clears the game's choices first)
  $port = 29100 + [array]::IndexOf($Runs, $run)
  $env:RETROHUB_AUTOTEST_DOLPHIN_ARGS = "-C Dolphin.Interface.ConfirmStop=False -C Dolphin.DSP.Volume=0 -C Dolphin.Core.EmulationSpeed=0" + $(if ($Mem) { " -C Dolphin.General.GDBPort=$port" } else { "" })
  $memOut = ""
  Remove-Item $log -ErrorAction SilentlyContinue
  $hubPid = [GcDesk]::Start("$d\RetroHub.exe", $d, $Desktop)
  $t0 = Get-Date; $dolphinPid = 0; $dstart = $null; $shotN = 0
  while (((Get-Date) - $t0).TotalSeconds -lt ($Seconds + 150)) {
    Start-Sleep 2
    $txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
    if ($txt -match "error:|play: NOT started|play: emulator process ended") { break }
    if (-not (Get-Process -Id $hubPid -ErrorAction SilentlyContinue)) { break }
    if (-not $dolphinPid) {
      $p = Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "Dolphin.exe" -and $_.ExecutablePath -like "$Work*" } | Select-Object -First 1
      if ($p) {
        $dolphinPid = $p.ProcessId; $dstart = Get-Date; Write-Host "dolphin pid $dolphinPid started by RetroHub (hidden desktop)"
        if ($Mem) {
          $memOut = (python (Join-Path $PSScriptRoot "gdb_read_mem.py") $port $RunSeconds $Mem.Split(",") 2>&1 | Out-String).Trim()
          Write-Host "gdb read: $memOut"
          break
        }
      }
      continue
    }
    $age = ((Get-Date) - $dstart).TotalSeconds
    if ($Shots -and $age -ge 10 -and [int]$age % 5 -lt 2) { [void][GcDesk]::Shot($Desktop, $dolphinPid, (Join-Path $Shots ("$name-t" + [int]$age + ".png"))) }
    if ($age -ge $Seconds) { break }
  }
  # the game ini RetroHub wrote, as Dolphin saw it when it started
  $gtxt = if (Test-Path $gini) { Get-Content $gini -Raw } else { "" }
  Write-Host "--- $GameId.ini (RetroHub entries) ---"
  ($gtxt -split "`r?`n") | Where-Object { $_ -match "RetroHub|^[0-9A-F]{8} [0-9A-F]{8}$" -or $_ -match "^\[" } | ForEach-Object { Write-Host "  $_" }
  $ltxt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
  $m = [regex]::Match($ltxt, "cheats: (\d+) code set\(s\) written")
  Write-Host ("log: " + $m.Value)
  if ($Mem) {
    $got = ([regex]::Match($memOut, "=([0-9A-Fa-f]+)")).Groups[1].Value.ToUpper()
    if (-not $got) { Fail "could not read game memory ($memOut)" }
    elseif ($cheats -ne "-" -and $got -eq $MemOn) { Pass "[$name] game memory $Mem = $got (cheat applied in the game)" }
    elseif ($cheats -eq "-" -and $got -ne $MemOn) { Pass "[$name] control: game memory $Mem = $got (not unlocked)" }
    else { Fail "[$name] game memory $Mem = $got (cheats: $cheats, unlocked value $MemOn)" }
  }
  if ($cheats -ne "-") {
    $n = ($cheats -split ";").Count
    if ($m.Success -and [int]$m.Groups[1].Value -eq $n) { Pass "RetroHub wrote $n cheat(s) into $GameId.ini through the Play path" } else { Fail "expected $n cheat(s) written, log says '$($m.Value)'" }
    if ((Test-Path $dini) -and (Get-Content $dini -Raw) -match "EnableCheats\s*=\s*True") { Pass "Dolphin.ini EnableCheats = True" } else { Fail "EnableCheats not set" }
  } else {
    if ($m.Success -and [int]$m.Groups[1].Value -eq 0) { Pass "control: 0 cheats written" } else { Fail "control run wrote cheats: '$($m.Value)'" }
  }
  if ($dolphinPid) {
    if ($Shots) { [void][GcDesk]::Shot($Desktop, $dolphinPid, (Join-Path $Shots "$name-final.png")) }
    [GcDesk]::Close($Desktop, $dolphinPid)   # WM_CLOSE: the same stop path as closing the window
    $w = 0; while ((Get-Process -Id $dolphinPid -ErrorAction SilentlyContinue) -and $w -lt 40) { Start-Sleep 1; $w++ }
    if (Get-Process -Id $dolphinPid -ErrorAction SilentlyContinue) { Fail "Dolphin pid $dolphinPid ignored WM_CLOSE"; Stop-Process -Id $dolphinPid -Force }   # our own child only
  } else { Fail "Dolphin never started" }
  Start-Sleep 3
  if (Get-Process -Id $hubPid -ErrorAction SilentlyContinue) { Stop-Process -Id $hubPid -Force }   # our own RetroHub copy
}
# after the last (control) run the file must hold no RetroHub entries
if ($Runs[-1] -match "=-$") {
  $gtxt = if (Test-Path $gini) { Get-Content $gini -Raw } else { "" }
  if ($gtxt -notmatch "RetroHub") { Pass "cheats off: no RetroHub entries left in $GameId.ini" } else { Fail "RetroHub entries remain after the control run" }
}
Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Write-Host "RESULT: $(if ($fail) { "FAIL ($fail)" } else { 'PASS' })  work: $Work  shots: $Shots"
exit $fail
