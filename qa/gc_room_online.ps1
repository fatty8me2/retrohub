# GameCube online through RetroHub's real room flow: N RetroHub instances (host + guests), each in its own folder with its own
# managed Dolphin and userdata, all on ONE private invisible desktop, talking to a local `npx wrangler dev` room server.
# RetroHub is driven by RETROHUB_AUTOTEST (src/hub/autotest.cpp), which presses the same code paths as the Join/Ready/Launch buttons;
# launch_online -> gc_begin -> Dolphin NetPlay UIA driver -> forwarders through the room all run unmodified. Muted, no focus.
# usage: gc_room_online.ps1 -Build <bin\Release> -DolphinDir <deps\dolphin> -Work <scratch> -Game mkdd -DiscDir <dir> [-GuestDiscDirs a,b,c]
#        [-Players 2] [-Seconds 120] [-Server ws://127.0.0.1:8793] [-WriteSave] [-Relay]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$DolphinDir, [Parameter(Mandatory)][string]$Work,
  [Parameter(Mandatory)][string]$Game, [Parameter(Mandatory)][string]$DiscDir, [string[]]$GuestDiscDirs = @(),
  [int]$Players = 2, [int]$Seconds = 120, [string]$Server = "ws://127.0.0.1:8793", [switch]$WriteSave, [switch]$Relay,
  [string]$Code = "", [string]$Desktop = "rhgc_room"
)
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
if (-not $Code) { $Code = -join ((65..90) | Get-Random -Count 6 | ForEach-Object { [char]$_ }) }
New-Item -ItemType Directory -Force $Work | Out-Null
[DeskLaunch]::Desk($Desktop)
$pids = @()
for ($i = 0; $i -lt $Players; $i++) {
  $role = if ($i -eq 0) { "host" } else { "join" }
  $d = Join-Path $Work "inst$i"
  if (Test-Path $d) { Remove-Item -Recurse -Force $d }
  New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
  Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
  robocopy $DolphinDir "$d\userdata\emulators\dolphin" /E /XD _downloads User /NFL /NDL /NJH /NJS /NP | Out-Null
  Set-Content "$d\userdata\emulators\dolphin\retrohub-install.txt" "autotest" -Encoding ascii
  $disc = if ($i -gt 0 -and $GuestDiscDirs.Count -ge $i) { $GuestDiscDirs[$i - 1] } else { $DiscDir }
  $v = ($d -replace '\\', '/')
  $json = '{"name":"At' + $i + '","rom_folders":["' + ($disc -replace '\\', '/') + '"],"server":"' + $Server + '","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}'
  Set-Content "$d\userdata\hub.json" $json -Encoding ascii
  $env:RETROHUB_AUTOTEST = $role; $env:RETROHUB_AUTOTEST_CODE = $Code; $env:RETROHUB_AUTOTEST_NAME = "At$i"
  $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_PLAYERS = "$Players"
  $env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"
  if ($WriteSave -and $i -eq 0) { $env:RETROHUB_AUTOTEST_WRITE = "1" } else { Remove-Item Env:RETROHUB_AUTOTEST_WRITE -ErrorAction SilentlyContinue }
  if ($Relay) { $env:HUBNET_FORCE_RELAY = "1" }
  $env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"
  $pids += [DeskLaunch]::Start("$d\RetroHub.exe", $d, $Desktop)
  Write-Host "started $role inst$i pid $($pids[-1]) room $Code"
  if ($i -eq 0) { Start-Sleep 8 }   # host first: it creates the room, guests join it in slot order
  else { Start-Sleep 6 }
}
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) { Start-Sleep 5; if (-not ($pids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })) { break } }
# evidence
for ($i = 0; $i -lt $Players; $i++) {
  Write-Host "=== inst$i autotest.log"; Get-Content "$Work\inst$i\userdata\autotest.log" -ErrorAction SilentlyContinue | Select-Object -Last 40
}
# kill only what we started: our RetroHub pids, and Dolphin/powershell processes whose image lives under $Work
$procs = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) -or ($_.Name -eq "powershell.exe" -and $_.CommandLine -like "*$Work*gc_netplay_driver*") }
foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
foreach ($p in $pids) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }
