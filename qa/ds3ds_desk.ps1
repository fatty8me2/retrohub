# Dot-sourced helper: start processes on a private invisible desktop (CreateDesktop + STARTUPINFO.lpDesktop), wait, exit code.
# Nothing here can show a window on the user's desktop. Inherits the caller's environment.
if (-not ([System.Management.Automation.PSTypeName]'DeskLaunch').Type) {
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class DeskLaunch {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI { public int cb; public string r, desk, title; public int x,y,xs,ys,xc,yc,fill,flags; public short show, res2; public IntPtr r2,i,o,e; }
  [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateDesktop(string n, IntPtr d, IntPtr dm, int f, uint a, IntPtr sa);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inh, uint fl, IntPtr env, string cwd, ref SI si, out PI pi);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
  public static void Desk(string n) { CreateDesktop(n, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero); }
  public static IntPtr Start(string cmdline, string cwd, string desk) {
    SI si = new SI(); si.cb = Marshal.SizeOf(si); si.desk = desk; PI pi;
    if (!CreateProcess(null, cmdline, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi)) throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
    return pi.hp;
  }
  public static bool Wait(IntPtr h, int ms) { return WaitForSingleObject(h, (uint)ms) == 0; }
  public static int ExitCode(IntPtr h) { uint c; GetExitCodeProcess(h, out c); return unchecked((int)c); }
}
"@
}
function New-HiddenDesktop([string]$Name) { [DeskLaunch]::Desk($Name) }
