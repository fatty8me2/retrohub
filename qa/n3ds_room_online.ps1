# 3DS online through RetroHub's real room flow: host + guest RetroHub instances (each with its own managed Azahar copy + its own
# NAND/MAC, so each has a different 3DS username and console identity) on ONE private invisible desktop, against a local
# `npx wrangler dev` room server. Driven by RETROHUB_AUTOTEST (src/hub/autotest.cpp). Muted, no focus. Kills only what it started.
# usage: n3ds_room_online.ps1 -Build <bin\Release> -AzaharDir <clean azahar dir, no user folder> -UserA <seeded user dir A> -UserB <user dir B>
#        -Work <scratch> -RomDir <dir with the game zip/.3ds> -Game pokex [-Seconds 150] [-Server ws://127.0.0.1:8793] [-Relay]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$AzaharDir, [Parameter(Mandatory)][string]$UserA,
  [Parameter(Mandatory)][string]$UserB, [Parameter(Mandatory)][string]$Work, [Parameter(Mandatory)][string]$RomDir,
  [string]$Game = "pokex", [int]$Seconds = 150, [string]$Server = "ws://127.0.0.1:8793", [switch]$Relay,
  [string]$Code = "", [string]$Desktop = "rh3ds_room"
)
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class DeskLaunch3 {
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
[DeskLaunch3]::Desk($Desktop)
$pids = @(); $users = @($UserA, $UserB)
for ($i = 0; $i -lt 2; $i++) {
  $role = if ($i -eq 0) { "host" } else { "join" }
  $d = Join-Path $Work "inst$i"
  # keep the managed Azahar dir between runs (same exe path => the firewall asks once); refresh everything else
  New-Item -ItemType Directory -Force "$d\userdata\emulators" | Out-Null
  Get-ChildItem $d -Exclude userdata -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
  Remove-Item "$d\userdata\autotest.log" -ErrorAction SilentlyContinue
  Copy-Item "$Build\*" $d -Recurse -Include *.exe, *.dll -Exclude hub_*.exe
  $az = "$d\userdata\emulators\azahar"
  if (-not (Test-Path "$az\azahar.exe")) {
    robocopy $AzaharDir $az /E /NFL /NDL /NJH /NJS /NP | Out-Null
    robocopy $users[$i] "$az\user" /E /NFL /NDL /NJH /NJS /NP | Out-Null
  }
  Set-Content "$az\retrohub-install.txt" "autotest" -Encoding ascii
  $v = ($d -replace '\\', '/')
  $json = '{"name":"At' + $i + '","rom_folders":["' + ($RomDir -replace '\\', '/') + '"],"server":"' + $Server + '","vault_dir":"' + $v + '/vault","sync_auto":false,"cloud_saves":false}'
  Set-Content "$d\userdata\hub.json" $json -Encoding ascii
  $env:RETROHUB_AUTOTEST = $role; $env:RETROHUB_AUTOTEST_CODE = $Code; $env:RETROHUB_AUTOTEST_NAME = "At$i"
  $env:RETROHUB_AUTOTEST_GAME = $Game; $env:RETROHUB_AUTOTEST_PLAYERS = "2"
  $env:RETROHUB_ALLOW_VERSION_MISMATCH = "1"; $env:RETROHUB_NO_UPDATE_CHECK = "1"
  if ($Relay) { $env:HUBNET_FORCE_RELAY = "1" } else { Remove-Item Env:HUBNET_FORCE_RELAY -ErrorAction SilentlyContinue; $env:HUBNET_LOOPBACK = "1" }
  $env:RETRORUN_MUTE = "1"; $env:SSB64_MUTE = "1"
  $pids += [DeskLaunch3]::Start("$d\RetroHub.exe", $d, $Desktop)
  Write-Host "started $role inst$i pid $($pids[-1]) room $Code"
  Start-Sleep 8
}
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) { Start-Sleep 5; if (-not ($pids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })) { break } }
for ($i = 0; $i -lt 2; $i++) {
  Write-Host "=== inst$i autotest.log"; Get-Content "$Work\inst$i\userdata\autotest.log" -ErrorAction SilentlyContinue | Select-Object -Last 30
  Write-Host "=== inst$i room log"; Get-Content "$Work\inst$i\userdata\netplay\3ds\azahar-room.out" -ErrorAction SilentlyContinue | Select-String "Network" | ForEach-Object { $_.Line }
  Write-Host "=== inst$i join driver"; Get-Content "$Work\inst$i\userdata\netplay\3ds\azahar_join_*.txt" -ErrorAction SilentlyContinue
}
# window titles on the hidden desktop belong to our processes only; kill what lives under $Work plus our RetroHub pids
$procs = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) -or ($_.Name -eq "powershell.exe" -and $_.CommandLine -like "*$Work*azahar_join*") }
foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
foreach ($p in $pids) { Stop-Process -Id $p -Force -ErrorAction SilentlyContinue }
