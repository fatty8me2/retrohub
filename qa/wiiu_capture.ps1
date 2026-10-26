# Runs ON the hidden desktop (started by wiiu_offline_play.ps1): lists the windows of one process and captures the biggest one with
# PrintWindow(PW_RENDERFULLCONTENT) into a PNG; optional -F12 also posts F12 to it (Cemu's own screenshot hotkey, which grabs the
# framebuffer even when PrintWindow returns black). Nothing here is visible to the user: the desktop has no display.
param([Parameter(Mandatory)][int]$TargetPid, [Parameter(Mandatory)][string]$Out, [switch]$F12, [string]$Log = "")
if ($Log) { Start-Transcript -Path $Log -Force | Out-Null }
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class Cap {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  public static List<IntPtr> Of(uint pid) {
    var v = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) v.Add(h); return true; }, IntPtr.Zero);
    return v;
  }
  public static int[] Size(IntPtr h) { RECT r; GetClientRect(h, out r); return new int[] { r.r - r.l, r.b - r.t }; }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
}
"@
$best = $null; $area = 0
foreach ($h in [Cap]::Of([uint32]$TargetPid)) {
  $s = [Cap]::Size($h); $a = $s[0] * $s[1]
  Write-Host ("window {0} {1}x{2} '{3}'" -f $h, $s[0], $s[1], [Cap]::Title($h))
  if ($a -gt $area) { $area = $a; $best = $h }
}
if (-not $best -or $area -le 0) { Write-Host "no window"; exit 2 }
$s = [Cap]::Size($best)
$bmp = New-Object System.Drawing.Bitmap $s[0], $s[1]
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [Cap]::PrintWindow($best, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
# how much of the picture is not black: a quick "did it really render" number
$nb = 0; $tot = 0
for ($y = 0; $y -lt $s[1]; $y += 8) { for ($x = 0; $x -lt $s[0]; $x += 8) { $c = $bmp.GetPixel($x, $y); $tot++; if ($c.R + $c.G + $c.B -gt 40) { $nb++ } } }
$bmp.Dispose()
Write-Host ("captured {0} PrintWindow={1} non-black={2}/{3}" -f $Out, $ok, $nb, $tot)
if ($F12) { [Cap]::PostMessage($best, 0x100, [IntPtr]0x7B, [IntPtr]0); Start-Sleep -Milliseconds 80; [Cap]::PostMessage($best, 0x101, [IntPtr]0x7B, [IntPtr]0); Write-Host "F12 posted" }
