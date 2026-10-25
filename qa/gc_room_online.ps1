# GameCube online through RetroHub's real room flow: N RetroHub instances (host + guests), each in its own folder with its own
# managed Dolphin and userdata, all on ONE private invisible desktop, talking to a local `npx wrangler dev` room server.
# RetroHub is driven by RETROHUB_AUTOTEST (src/hub/autotest.cpp), which presses the same code paths as the Join/Ready/Launch buttons;
# launch_online -> gc_begin -> Dolphin NetPlay UIA driver -> forwarders through the room all run unmodified. Muted, no focus.
# usage: gc_room_online.ps1 -Build <bin\Release> -DolphinDir <deps\dolphin> -Work <scratch> -Game mkdd -DiscDir <dir> [-GuestDiscDirs a,b,c]
#        [-Players 2] [-Seconds 120] [-Server ws://127.0.0.1:8793] [-WriteSave] [-Relay]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$DolphinDir, [Parameter(Mandatory)][string]$Work,
  [Parameter(Mandatory)][string]$Game, [Parameter(Mandatory)][string]$DiscDir, [string[]]$GuestDiscDirs = @(),
  [int]$Players = 2, [int]$Seconds = 120, [string]$Server = "ws://127.0.0.1:8793", [switch]$WriteSave, [switch]$Relay, [switch]$SeedCards,
  [string]$Code = "", [string]$Desktop = "rhgc_room", [int]$KillHostAfter = 0
)
. "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class DeskLaunch {
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
# the Worker only accepts room codes from A-H J-N P-Z 2-9 (no I, O, 0, 1)
if (-not $Code) { $alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray(); $Code = -join (1..6 | ForEach-Object { $alpha | Get-Random }) }
New-Item -ItemType Directory -Force $Work | Out-Null
[DeskLaunch]::Desk($Desktop)
Add-Type -TypeDefinition @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class WinList {
  delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenDesktop(string n, int f, bool i, uint a);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr d, EP p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static int Close(string desk, int pid, string titlePart) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); int n = 0;
    EnumDesktopWindows(d, (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) { var t = new StringBuilder(200); GetWindowText(h, t, 200); if (t.ToString().Contains(titlePart)) { PostMessage(h, 0x10, IntPtr.Zero, IntPtr.Zero); n++; } } return true; }, IntPtr.Zero);
    return n;
  }
  public static string Of(string desk, int pid) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); var r = new List<string>();
    EnumDesktopWindows(d, (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p == pid) { var t = new StringBuilder(200); var c = new StringBuilder(100); GetWindowText(h, t, 200); GetClassName(h, c, 100); r.Add((IsWindowVisible(h) ? "V " : "h ") + c + " '" + t + "'"); } return true; }, IntPtr.Zero);
    return string.Join("; ", r);
  }
}
"@
$pids = @()
for ($i = 0; $i -lt $Players; $i++) {
  $role = if ($i -eq 0) { "host" } else { "join" }
  $d = Join-Path $Work "inst$i"
  if (Test-Path $d) { Remove-Item -Recurse -Force $d }
  New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
  Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
  robocopy $DolphinDir "$d\userdata\emulators\dolphin" /E /XD _downloads User /NFL /NDL /NJH /NJS /NP | Out-Null
  Set-Content "$d\userdata\emulators\dolphin\retrohub-install.txt" "autotest" -Encoding ascii
  if ($SeedCards) {   # sentinel file in every player's real card folder (guests' must stay untouched; the host's only changes with -WriteSave)
    $cardDir = "$d\userdata\emulators\dolphin\User\GC\RetroHub\gc_gm4e01-0\GCI"
    New-Item -ItemType Directory -Force $cardDir | Out-Null
    Set-Content "$cardDir\sentinel_inst$i.bin" ("sentinel " + $i) -Encoding ascii
  }
  $disc = if ($i -gt 0 -and $GuestDiscDirs.Count -ge $i) { $GuestDiscDirs[$i - 1] } else { $DiscDir }
  $v = ($d -replace '\\', '/')
  $json = '{"name":"qa-At' + $i + '","rom_folders":["' + ($disc -replace '\\', '/') + '"],"server":"' + $Server + '","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}'
  Set-Content "$d\userdata\hub.json" $json -Encoding ascii
  $env:RETROHUB_AUTOTEST = $role; $env:RETROHUB_AUTOTEST_CODE = $Code; $env:RETROHUB_AUTOTEST_NAME = "qa-At$i"
  $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_PLAYERS = "$Players"
  $env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"
  if ($WriteSave -and $i -eq 0) { $env:RETROHUB_AUTOTEST_WRITE = "1" } else { Remove-Item Env:RETROHUB_AUTOTEST_WRITE -ErrorAction SilentlyContinue }
  $env:RETROHUB_QA = "1"   # QA switches (HUBNET_LOOPBACK) need this
  if ($Relay) { $env:HUBNET_FORCE_RELAY = "1"; Remove-Item Env:HUBNET_LOOPBACK -ErrorAction SilentlyContinue }   # relay path: UDP punching off
  else { $env:HUBNET_LOOPBACK = "1"; Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue }   # direct path on one PC: no STUN, loopback candidates
  $env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"
  $pids += [DeskLaunch]::Start("$d\RetroHub.exe", $d, $Desktop)
  Write-Host "started $role inst$i pid $($pids[-1]) room $Code"
  if ($i -eq 0) { Start-Sleep 8 }   # host first: it creates the room, guests join it in slot order
  else { Start-Sleep 6 }
}
$end = (Get-Date).AddSeconds($Seconds)
# -KillHostAfter <s>: host-leaves QA. Once the first guest's Dolphin has run <s> seconds, everything of the HOST (inst0: RetroHub, Dolphin,
# driver; own pids only) is killed. The guest must log "host left", close Dolphin gracefully (WM_CLOSE) and sync the vault.
$killed = $null; $gDol = $null
function GuestDol { Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "Dolphin.exe" -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith("$Work\inst1", [StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1 }
while ((Get-Date) -lt $end) {
  Start-Sleep 3
  if ($KillHostAfter -gt 0) {
    if (-not $killed) {
      $g = GuestDol
      if ($g -and -not $gDol) { $gDol = Get-Date; Write-Host "guest dolphin pid $($g.ProcessId) running" }
      if ($gDol -and ((Get-Date) - $gDol).TotalSeconds -ge $KillHostAfter) {
        Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith("$Work\inst0", [StringComparison]::OrdinalIgnoreCase) -or ($_.Name -eq "powershell.exe" -and $_.CommandLine -like "*$Work\inst0*") } | ForEach-Object { Write-Host "killing host-side pid $($_.ProcessId) $($_.Name)"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        $killed = Get-Date; $end = (Get-Date).AddSeconds(60)
      }
    } elseif ((GuestDol) -and ((Get-Date) - $killed).TotalSeconds -ge 8 -and -not $listed) {
      $listed = $true; Write-Host ("guest dolphin windows 8s after the host died: " + [WinList]::Of($Desktop, (GuestDol).ProcessId))
      if ($env:QA_PROBE_CLOSE) { Write-Host ("probe: WM_CLOSE to NetPlay -> " + [WinList]::Close($Desktop, (GuestDol).ProcessId, "NetPlay")); Start-Sleep 2; Write-Host ("after: " + [WinList]::Of($Desktop, (GuestDol).ProcessId)) }
    } elseif (-not (GuestDol)) { Start-Sleep 5; break }
    continue
  }
  if (-not ($pids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })) { break }
}
if ($KillHostAfter -gt 0) {
  $gl = Get-Content "$Work\inst1\userdata\autotest.log" -Raw -ErrorAction SilentlyContinue
  Write-Host ("HOSTLEFT-CHECK killed=" + [bool]$killed + " host-left-logged=" + ($gl -match "host left") + " toast=" + ($gl -match " left") + " guest-dolphin-closed=" + (-not (GuestDol)))
}
# evidence
for ($i = 0; $i -lt $Players; $i++) {
  Write-Host "=== inst$i autotest.log"; Get-Content "$Work\inst$i\userdata\autotest.log" -ErrorAction SilentlyContinue | Select-Object -Last 40
}
if ($SeedCards) {
  for ($i = 0; $i -lt $Players; $i++) {
    $cardDir = "$Work\inst$i\userdata\emulators\dolphin\User\GC\RetroHub\gc_gm4e01-0\GCI"
    Write-Host "=== inst$i real card folder after session:"; Get-ChildItem $cardDir -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("  " + $_.Name + " " + $_.Length + " bytes") }
    Write-Host ("  WriteSaveData in Dolphin.ini: " + ((Select-String -Path "$Work\inst$i\userdata\emulators\dolphin\User\Config\Dolphin.ini" -Pattern "WriteSaveData" -ErrorAction SilentlyContinue | ForEach-Object { $_.Line.Trim() }) -join ","))
  }
}
# kill only what we started: our RetroHub pids, and Dolphin/powershell processes whose image lives under $Work
$procs = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) -or ($_.Name -eq "powershell.exe" -and $_.CommandLine -like "*$Work*gc_netplay_driver*") }
foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
foreach ($p in $pids) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }
