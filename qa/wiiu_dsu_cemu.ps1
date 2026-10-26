# Hidden-desktop check that Cemu connects to the RetroHub DSU server and (optionally) boots a game with P2 on DSU.
# Usage: wiiu_dsu_cemu.ps1 -CemuDir <scratch Cemu copy> -DsuFeed <dsu_feed.exe> -ProfilesFrom <dir with portable\controllerProfiles+settings.xml>
#        [-Game <.wua>] [-Seconds 90] [-Shot <out.png>]
# NEVER point -CemuDir at the user's real Cemu: portable\ is overwritten. Everything runs on a private invisible desktop.
param([Parameter(Mandatory)][string]$CemuDir, [Parameter(Mandatory)][string]$DsuFeed, [Parameter(Mandatory)][string]$ProfilesFrom,
      [string]$Game = "", [int]$Seconds = 90, [int]$Wait = 100, [string]$Shot = "")
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ds3ds_desk.ps1")
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class DeskShot {
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenDesktop(string n, int f, bool i, uint a);
  [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr d, EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int l, t, r, b; }
  public static List<string> Windows(string desk) {
    var res = new List<string>(); IntPtr d = OpenDesktop(desk, 0, false, 0x1FF);
    EnumDesktopWindows(d, (h, l) => { var sb = new System.Text.StringBuilder(256); GetWindowText(h, sb, 256); RECT r; GetWindowRect(h, out r);
      if (sb.Length > 0) res.Add(h.ToInt64() + "|" + sb + "|" + (r.r - r.l) + "x" + (r.b - r.t)); return true; }, IntPtr.Zero);
    return res;
  }
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc e, IntPtr l);
  /* Cemu's own screenshot hotkey (Ctrl+F12) posted to the Cemu window tree: it renders the real GL/Vulkan frame to <portable>\screenshots. */
  public static void Hotkey(string desk, string titlePart) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); var all = new List<IntPtr>();
    EnumDesktopWindows(d, (h, l) => { var sb = new System.Text.StringBuilder(256); GetWindowText(h, sb, 256);
      if (sb.ToString().Contains(titlePart)) { all.Add(h); EnumChildWindows(h, (c, x) => { all.Add(c); return true; }, IntPtr.Zero); } return true; }, IntPtr.Zero);
    foreach (var h in all) {
      PostMessage(h, 0x100, (IntPtr)0x11, (IntPtr)0x001D0001);   // WM_KEYDOWN VK_CONTROL
      PostMessage(h, 0x100, (IntPtr)0x7B, (IntPtr)0x00580001);   // WM_KEYDOWN VK_F12
      PostMessage(h, 0x101, (IntPtr)0x7B, unchecked((IntPtr)0xC0580001));
      PostMessage(h, 0x101, (IntPtr)0x11, unchecked((IntPtr)0xC01D0001));
    }
  }
  public static bool Shot(string desk, string titlePart, string path) {
    IntPtr d = OpenDesktop(desk, 0, false, 0x1FF); IntPtr found = IntPtr.Zero; int w = 0, hh = 0;
    EnumDesktopWindows(d, (h, l) => { var sb = new System.Text.StringBuilder(256); GetWindowText(h, sb, 256); RECT r; GetWindowRect(h, out r);
      if (sb.ToString().Contains(titlePart) && r.r - r.l > 200) { found = h; w = r.r - r.l; hh = r.b - r.t; return false; } return true; }, IntPtr.Zero);
    if (found == IntPtr.Zero) return false;
    using (var bmp = new Bitmap(w, hh)) { using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(found, dc, 2); g.ReleaseHdc(dc); } bmp.Save(path, ImageFormat.Png); }
    return true;
  }
}
"@
$name = "wiiu_dsu_" + [guid]::NewGuid().ToString("N").Substring(0, 8)
New-HiddenDesktop $name
# profiles + settings go into the SCRATCH copy only
$portable = Join-Path $CemuDir "portable"
New-Item -ItemType Directory -Force $portable | Out-Null
Copy-Item (Join-Path $ProfilesFrom "portable\*") $portable -Recurse -Force
$log = Join-Path $CemuDir "dsu_feed.log"
Remove-Item $log -ErrorAction SilentlyContinue
$feed = [DeskLaunch]::Start("`"$DsuFeed`" $($Seconds + 30) 26760 `"$log`"", $CemuDir, $name)
Start-Sleep 2
$args = if ($Game) { "-g `"$Game`"" } else { "" }
$env:SDL_AUDIODRIVER = "dummy"
$cemu = [DeskLaunch]::Start("`"$(Join-Path $CemuDir 'Cemu.exe')`" $args", $CemuDir, $name)
$ok = $false
for ($i = 0; $i -lt $Seconds; $i += 3) {
    Start-Sleep 3
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
    if ($t -match "clients=[1-9]") { $ok = $true; if (-not $Game) { break } }
    if ($Game -and $ok -and $i -ge $Wait) { break }
}
"windows on hidden desktop:"; [DeskShot]::Windows($name)
if ($Game) { foreach ($n in 1..3) { [DeskShot]::Hotkey($name, "Cemu"); Start-Sleep 20 } }   # three frames, 20 s apart
if ($Shot) {   # screen grab from inside the hidden desktop (GL/Vulkan surfaces come out blank with PrintWindow)
    $scr = [DeskLaunch]::Start("powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'desk_screen.ps1')`" -Out `"$($Shot -replace '\.png$','_screen.png')`"", $CemuDir, $name)
    [void][DeskLaunch]::Wait($scr, 30000)
}
if ($Shot) { $done = $false; foreach ($t in "Cemu", "Super Mario") { if ([DeskShot]::Shot($name, $t, $Shot)) { $done = $true; break } }; "shot: $done -> $Shot" }
if (Test-Path $log) { "--- dsu_feed.log"; Get-Content $log }
# kill only the two processes started here (own PIDs via the handles)
foreach ($h in @($cemu, $feed)) { try { Add-Type -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool TerminateProcess(IntPtr h, uint c);' -Name K -Namespace W -ErrorAction SilentlyContinue; [W.K]::TerminateProcess($h, 0) | Out-Null } catch {} }
if ($ok) { "RESULT: Cemu connected to the DSU server" } else { "RESULT: no DSU client seen"; exit 1 }

