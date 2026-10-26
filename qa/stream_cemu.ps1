# Real-capture remote play test: a SCRATCH copy of Cemu runs a game on a private hidden desktop, streamrun --host captures it
# (also on that desktop), a headless streamrun --guest on loopback receives it. Nothing is shown on the user's screen, Cemu is muted
# (TVVolume 0). Run through the local-worker helper so RETROHUB_TEST_SERVER is set:
#   node qa\online_local.mjs 12010 powershell -NoProfile -File qa\stream_cemu.ps1 -Build <bin\Release> -CemuSrc <Cemu_2.6 dir> -Game <x.wua> [-Seconds 30]
param(
  [Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$CemuSrc, [Parameter(Mandatory)][string]$Game,
  [string]$Work = "C:\rhq\cemu_stream", [int]$Seconds = 30, [int]$BootWait = 90, [string]$HostArgs = "", [string]$GuestArgs = "",
  [string]$Desktop = "rhstream_cemu"
)
. "$PSScriptRoot\_guard.ps1"
$Server = $env:RETROHUB_TEST_SERVER
if (-not $Server) { Write-Error "RETROHUB_TEST_SERVER not set (use qa\online_local.mjs)"; exit 2 }
Assert-LocalServer $Server
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class HD {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenDesktop(string n, int f, bool i, uint a);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr d, EP p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static int Start(string cmdline, string cwd, string desk) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, cmdline, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
    return pi.pid;
  }
  public static string Windows(string desk) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); var sb = new StringBuilder();
    if (d == IntPtr.Zero) return "";
    EnumDesktopWindows(d, (h, l) => { int pid; GetWindowThreadProcessId(h, out pid); RECT r; GetClientRect(h, out r);
      var t = new StringBuilder(256); GetWindowText(h, t, 256);
      if (IsWindowVisible(h)) sb.AppendLine(pid + "|" + (r.r - r.l) + "x" + (r.b - r.t) + "|" + t); return true; }, IntPtr.Zero);
    return sb.ToString();
  }
}
"@

$exe = Join-Path (Resolve-Path $Build) "streamrun.exe"
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force "$Work\portable" | Out-Null
Copy-Item "$CemuSrc\Cemu.exe" $Work
Copy-Item "$CemuSrc\resources" "$Work\resources" -Recurse
@"
<?xml version="1.0" encoding="UTF-8"?>
<content>
	<logflag>0</logflag>
	<mlc_path/>
	<disable_screensaver>true</disable_screensaver>
	<check_update>false</check_update>
	<receive_untested_updates>false</receive_untested_updates>
	<use_discord_presence>false</use_discord_presence>
	<vk_warning>false</vk_warning>
	<gp_download>false</gp_download>
	<fullscreen>false</fullscreen>
	<Audio><api>3</api><delay>2</delay><TVChannels>1</TVChannels><TVVolume>0</TVVolume><PadVolume>0</PadVolume></Audio>
</content>
"@ | Set-Content "$Work\portable\settings.xml" -Encoding utf8
[HD]::Desk($Desktop)
$cemuPid = [HD]::Start("`"$Work\Cemu.exe`" -g `"$Game`"", $Work, $Desktop)
"cemu pid $cemuPid"
$found = $false; $t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $BootWait) {
  Start-Sleep 2
  $w = [HD]::Windows($Desktop) -split "`n" | Where-Object { $_ -like "$cemuPid|*" }
  if (-not (Get-Process -Id $cemuPid -ErrorAction SilentlyContinue)) { "Cemu exited early"; break }
  $big = $w | Where-Object { $_ -match '\|(\d+)x(\d+)\|' -and [int]$Matches[1] -gt 600 -and [int]$Matches[2] -gt 300 }
  if ($big) { $found = $true; $big; break }
}
if (-not $found) { "no Cemu window"; Get-Process -Id $cemuPid -ErrorAction SilentlyContinue | Stop-Process -Force; exit 3 }
Start-Sleep 60   # let the game get past boot logos so there is motion

$out = Join-Path $Work "out"; New-Item -ItemType Directory -Force $out | Out-Null
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray()
$room = (-join (1..6 | ForEach-Object { $alpha | Get-Random })) + "-1"
$hostStats = "$out\host.json"; $guestStats = "$out\guest.json"
$hostPid = [HD]::Start("cmd.exe /c `"`"$exe`" --host --capture-hwnd $cemuPid --net $Server --room $room --slot 0 --duration $($Seconds + 6) --stats-file `"$hostStats`" --pad-port 26999 $HostArgs 2> `"$out\host.log`"`"", $Work, $Desktop)
Start-Sleep -Milliseconds 1500
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe; $psi.Arguments = "--guest --headless --test-input --net $Server --room $room --slot 1 --duration $Seconds --stats-file `"$guestStats`" $GuestArgs"
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
$g = New-Object System.Diagnostics.Process; $g.StartInfo = $psi; $null = $g.Start()
$errTask = $g.StandardError.ReadToEndAsync(); $null = $g.StandardOutput.ReadToEndAsync()
$null = $g.WaitForExit(($Seconds + 60) * 1000)
Start-Sleep 8
$hp = Get-Process -Id $hostPid -ErrorAction SilentlyContinue; if ($hp) { $hp | Stop-Process -Force }
Get-Process -Id $cemuPid -ErrorAction SilentlyContinue | Stop-Process -Force   # only the PID this script started
"--- host.log"; Get-Content "$out\host.log" -ErrorAction SilentlyContinue
"--- guest.log"; $errTask.Result
"--- host.json"; Get-Content $hostStats -ErrorAction SilentlyContinue
"--- guest.json"; Get-Content $guestStats -ErrorAction SilentlyContinue
