# Verifies Cemu parses a RetroHub-style SDL controller profile ("<index>_<SDL GUID>"): runs a SCRATCH copy of Cemu on a hidden
# desktop (never the user's real install), closes it gracefully, then reads back the controllerProfiles it saved.
# Usage: qa\cemu_sdl_readback.ps1 -CemuSrc C:\path\to\Cemu_2.6   (only Cemu.exe + resources are copied)
param([Parameter(Mandatory)][string]$CemuSrc)
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:TEMP ("cemu_sdl_qa_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
Copy-Item "$CemuSrc\Cemu.exe" $work
Copy-Item "$CemuSrc\resources" "$work\resources" -Recurse
$cp = "$work\portable\controllerProfiles"
New-Item -ItemType Directory $cp -Force | Out-Null
$uuid = '0_030000005e040000ff02000000007200'
$xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<emulated_controller>
	<type>Wii U GamePad</type>
	<controller>
		<api>SDLController</api>
		<uuid>$uuid</uuid>
		<display_name>Controller 1</display_name>
		<rumble>0</rumble>
		<axis><deadzone>0.25</deadzone><range>1</range></axis>
		<rotation><deadzone>0.25</deadzone><range>1</range></rotation>
		<trigger><deadzone>0.25</deadzone><range>1</range></trigger>
		<mappings>
			<entry><mapping>1</mapping><button>1</button></entry>
			<entry><mapping>7</mapping><button>9</button></entry>
			<entry><mapping>11</mapping><button>11</button></entry>
			<entry><mapping>17</mapping><button>39</button></entry>
		</mappings>
	</controller>
</emulated_controller>
"@
Set-Content "$cp\controller0.xml" $xml -Encoding utf8
Set-Content "$work\portable\settings.xml" '<?xml version="1.0" encoding="UTF-8"?><content><check_update>false</check_update><use_discord_presence>false</use_discord_presence><Audio><api>3</api><TVVolume>0</TVVolume><PadVolume>0</PadVolume><InputVolume>0</InputVolume></Audio></content>' -Encoding utf8
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class HD {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenDesktop(string n, int f, bool inh, uint a);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr d, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static int Start(string exe, string cwd, string desk) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, "\"" + exe + "\"", IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
    return pi.pid;
  }
  public static int Close(string desk, int pid) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); int n = 0;
    EnumDesktopWindows(d, (h, l) => { int p; GetWindowThreadProcessId(h, out p); if (p == pid) { PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); n++; } return true; }, IntPtr.Zero);
    return n;
  }
}
"@
$desk = "cemu_sdl_qa_" + $PID
[HD]::Desk($desk)
$mypid = [HD]::Start("$work\Cemu.exe", $work, $desk)
Start-Sleep 12
"WM_CLOSE sent to {0} window(s)" -f [HD]::Close($desk, $mypid)
for ($i = 0; $i -lt 20 -and (Get-Process -Id $mypid -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
if (Get-Process -Id $mypid -ErrorAction SilentlyContinue) { Stop-Process -Id $mypid -Force; "had to stop pid $mypid" }   # our own pid only
"--- controllerProfiles after Cemu ran ---"
Get-ChildItem $cp | ForEach-Object { $_.Name }
Get-Content "$cp\controller0.xml" -ErrorAction SilentlyContinue
"scratch: $work"
