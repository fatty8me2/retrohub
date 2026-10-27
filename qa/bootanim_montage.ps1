# Cropped contact sheet from qa\bootanim_shots.ps1 output: one row per system, centre crop of each time. Usage: qa\bootanim_montage.ps1 <dir> <systems csv> <times csv> <out.png>
param([string]$Dir, [string]$Systems, [string]$Times, [string]$OutFile)
Add-Type -AssemblyName System.Drawing
$sy = $Systems.Split(","); $ti = $Times.Split(",")
$cw = 420; $ch = 400
$sheet = New-Object System.Drawing.Bitmap ($cw * $ti.Count), ($ch * $sy.Count)
$g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::Black)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
for ($r = 0; $r -lt $sy.Count; $r++) { for ($c = 0; $c -lt $ti.Count; $c++) {
  $f = Join-Path $Dir "boot_$($sy[$r])_$($ti[$c]).png"
  if (-not (Test-Path $f)) { continue }
  $im = [System.Drawing.Image]::FromFile($f)
  $src = New-Object System.Drawing.Rectangle 340, 120, 600, 570
  $dst = New-Object System.Drawing.Rectangle ($c * $cw), ($r * $ch), $cw, $ch
  $g.DrawImage($im, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel); $im.Dispose()
} }
$g.Dispose(); $sheet.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
