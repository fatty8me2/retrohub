# GameCube single-player through RetroHub's REAL launch path: RETROHUB_AUTOTEST=play presses the Play button's code (play_entry ->
# launch_external -> begin_session -> Dolphin -> end_session), on a private invisible desktop, muted, no network.
# Session 1: Dolphin boots the game; a card file is dropped into Dolphin's GCI folder (stand-in for the game's own save, see below),
#   Esc is posted (best effort), then WM_CLOSE ends Dolphin; RetroHub must log end_session and the file must be in the vault (same hash).
# Session 2: emulator card folder emptied (a "second PC"); RetroHub must restore the card from the vault at launch.
# Not automatable: making the GAME write the card. Dolphin ignores all injected input here (scripted .dtm movie via -Movie, pad
# expressions via -PadA; see qa\gc_make_dtm.py) and Esc is a DInput hotkey, so the stop uses WM_CLOSE.
# usage: gc_save_exit.ps1 -Build <bin\Release> -DolphinDir <deps\dolphin> -Work <scratch> -DiscDir <dir with an extracted disc> -Game <room id, e.g. melee>
#        [-GameId GALE01] [-EscAfter 15] [-Shots <dir>] [-DebugLog]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$DolphinDir, [Parameter(Mandatory)][string]$Work,
  [Parameter(Mandatory)][string]$DiscDir, [string]$GameId = "GALE01", [string]$Game = "", [int]$MovieSeconds = 240,
  [int]$EscAfter = 15, [string]$Pattern = "amash", [string]$Desktop = "rhgc_save", [string]$Shots = "", [string]$DolphinExtra = "", [int]$Lead = 600, [int]$CloseAfter = 0, [switch]$DebugLog, [string]$PadA = "", [switch]$Movie
)
$ErrorActionPreference = "Stop"
$Work = [IO.Path]::GetFullPath($Work)   # guard: the scratch dir must be a real, separate folder (it is wiped and its processes are stopped)
if ($Work.Length -lt 8 -or (Test-Path (Join-Path $Work ".git")) -or (Test-Path (Join-Path $Work "CMakeLists.txt"))) { throw "-Work must be a dedicated scratch folder, not $Work" }
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
New-Item -ItemType Directory -Force $Work | Out-Null
$d = Join-Path $Work "inst0"
Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }   # leftovers of an aborted run
Start-Sleep 2
if (Test-Path $d) { Remove-Item -Recurse -Force $d }
New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
robocopy $DolphinDir "$d\userdata\emulators\dolphin" /E /XD _downloads User /NFL /NDL /NJH /NJS /NP | Out-Null
Set-Content "$d\userdata\emulators\dolphin\retrohub-install.txt" "autotest" -Encoding ascii
if ($DebugLog) {   # Dolphin file log (flushed on a clean exit) for diagnosing movie / input problems
  $cfg = "$d\userdata\emulators\dolphin\User\Config"; New-Item -ItemType Directory -Force $cfg | Out-Null
  Set-Content "$cfg\Logger.ini" "[Options]`nVerbosity = 5`nWriteToFile = True`nWriteToConsole = False`nWriteToWindow = False`n[Logs]`nMOVIE = True`nCORE = True`nSI = True`nBOOT = True`nMASTER = True" -Encoding ascii
}
if ($PadA) {   # QA pad: button A driven by a Dolphin controller expression (constants/timers work without any device); RetroHub copies it over its own GCPadNew.ini
  $padIni = Join-Path $Work "GCPadNew.qa.ini"
  Set-Content $padIni ("[GCPad1]`nDevice = XInput/0/Gamepad`nButtons/A = " + $PadA + "`n") -Encoding ascii
  $env:RETROHUB_AUTOTEST_GCPAD_INI = $padIni
}
$dtm = Join-Path $Work "input.dtm"
python (Join-Path $PSScriptRoot "gc_make_dtm.py") $dtm $GameId --seconds $MovieSeconds --pattern $Pattern --lead $Lead | Out-Null
$v = ($d -replace '\\', '/')
Set-Content "$d\userdata\hub.json" ('{"name":"qa-GcSave","rom_folders":["' + ($DiscDir -replace '\\', '/') + '"],"server":"ws://127.0.0.1:9/","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}') -Encoding ascii
if (-not $Game) {   # discover the room id from a dry list: run once in list mode (autotest logs every library game)
  $Game = "?"
}
$env:RETROHUB_AUTOTEST = "play"; $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_NAME = "qa-GcSave"
$movieArg = if (-not $Movie) { "" } else { "-m `"$dtm`"" }
$env:RETROHUB_AUTOTEST_DOLPHIN_ARGS = "$movieArg -C Dolphin.Core.EmulationSpeed=0 -C Dolphin.Interface.ConfirmStop=False $DolphinExtra"
$env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_NO_UPDATE_CHECK = "1"; $env:RETROHUB_QA = "1"
$env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"; $env:HUBNET_LOOPBACK = "1"
[GcDesk]::Desk($Desktop)
$log = "$d\userdata\autotest.log"
$cardDir = "$d\userdata\emulators\dolphin\User\GC\RetroHub\gc_$($Game.Replace('-','_'))"   # refined below from the log if needed
function Find-CardDir { $x = Get-ChildItem "$d\userdata\emulators\dolphin\User\GC\RetroHub" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1; if ($x) { Join-Path $x.FullName "GCI" } else { "" } }
function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }

# One RetroHub "Play" session. -Inject: after boot, drop a card file into the emulator's GCI folder (stands in for the game's save when no
# input can be delivered). -ExpectHash: the emulator folder must already hold this file (restored from the vault by begin_session).
function Run-Session([string]$Label, [switch]$Inject, [string]$ExpectHash = "") {
  Write-Host "=== session: $Label"
  Remove-Item $log -ErrorAction SilentlyContinue
  $hubPid = [GcDesk]::Start("$d\RetroHub.exe", $d, $Desktop)
  $t0 = Get-Date; $dolphinPid = 0; $dstart = $null; $stage = 0
  while (((Get-Date) - $t0).TotalSeconds -lt ($EscAfter + 120)) {
    Start-Sleep 2
    $txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
    if ($txt -match "error:|play: NOT started|play: emulator process ended") { break }
    if (-not (Get-Process -Id $hubPid -ErrorAction SilentlyContinue)) { break }
    if (-not $dolphinPid) {
      $p = Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "Dolphin.exe" -and $_.ExecutablePath -like "$Work*" } | Select-Object -First 1
      if ($p) { $dolphinPid = $p.ProcessId; $dstart = Get-Date; Write-Host "dolphin pid $dolphinPid started by RetroHub (hidden desktop)" }
      continue
    }
    $age = ((Get-Date) - $dstart).TotalSeconds
    if ($Shots -and [int]$age % 6 -lt 2) { New-Item -ItemType Directory -Force $Shots | Out-Null; [void][GcDesk]::Shot($Desktop, $dolphinPid, (Join-Path $Shots ("$Label-t" + [int]$age + ".png"))) }
    if ($stage -eq 0 -and $age -ge $EscAfter) {
      $cd = Find-CardDir
      if ($ExpectHash) {
        $f = Get-ChildItem $cd -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($f -and (Sha $f.FullName) -eq $ExpectHash) { Write-Host "  restored from vault into the emulator card folder: $($f.Name) $ExpectHash" } else { Fail "card was not restored from the vault into the emulator folder" }
      }
      if ($Inject) {
        $rnd = New-Object byte[] 24640; (New-Object System.Random 7).NextBytes($rnd)
        [IO.File]::WriteAllBytes((Join-Path $cd "01-GALE-QAsave.gci"), $rnd)
        $script:injHash = Sha (Join-Path $cd "01-GALE-QAsave.gci")
        Write-Host "  wrote card file 01-GALE-QAsave.gci $($script:injHash) into the emulator folder"
      }
      Write-Host ("  Esc -> " + ([GcDesk]::Esc($Desktop, @($dolphinPid)).Split('|')[0]))
      $stage = 1; $escAt = Get-Date
    }
    if ($stage -eq 1 -and ((Get-Date) - $escAt).TotalSeconds -ge 10) {
      Write-Host "  Esc ignored (Dolphin's Stop hotkey reads the physical keyboard, which a hidden desktop does not have) -> WM_CLOSE on the render window, the same stop path as closing it"
      [GcDesk]::Close($Desktop, $dolphinPid); $stage = 2
    }
  }
  $txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
  Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 8 | ForEach-Object { Write-Host "  log: $_" }
  if ($txt -notmatch "play: emulator process ended") { Fail "RetroHub never saw Dolphin exit" }
  if ($txt -notmatch "end_session:") { Fail "no end_session (vault copy) after exit" }
  if ($txt -notmatch "closed\.") { Fail "exit was not reported as a clean close" }
  if ($dolphinPid -and (Get-Process -Id $dolphinPid -ErrorAction SilentlyContinue)) { Fail "Dolphin still running"; Stop-Process -Id $dolphinPid -Force }
  if (-not $dolphinPid) { Fail "Dolphin never started" }
}

Run-Session "1-save" -Inject
$vf = Get-ChildItem "$d\vault" -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "\\GCI\\" }
if (-not $vf) { Fail "card file did not reach the vault" } else { $vf | ForEach-Object { Write-Host ("  vault: " + $_.FullName.Substring($d.Length) + " " + $_.Length + " " + (Sha $_.FullName)) } }
if ($vf -and (Sha $vf[0].FullName) -ne $script:injHash) { Fail "vault copy differs from the card file" }

# second PC: empty emulator card folder, the vault restores it when RetroHub starts the game
$cardDir = Find-CardDir   # never fall back to the current directory: an empty path made Get-ChildItem list (and delete) the cwd's files
if ($cardDir -and (Test-Path $cardDir) -and $cardDir.StartsWith($d, [StringComparison]::OrdinalIgnoreCase)) { Get-ChildItem $cardDir -File | Remove-Item -Force } else { Fail "card folder not found; skipped emptying it" }
Run-Session "2-restore" -ExpectHash $script:injHash
Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
if ($fail -eq 0) { Write-Host "GC VAULT+EXIT QA PASS" } else { Write-Host "GC VAULT+EXIT QA FAIL ($fail)"; exit 1 }
