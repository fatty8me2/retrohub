# Makes a contact sheet (PNG) of several screenshots: crops the middle of each (where the centre case is) and tiles them in a grid.
# Usage: qa\contact_sheet.ps1 out.png a.png b.png ... [-Cols 2] [-Full]   (-Full keeps the whole frame, tabs and hints included, instead of the middle crop)
param([Parameter(Mandatory = $true, Position = 0)][string]$Out, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Files, [int]$Cols = 2, [switch]$Full)
Add-Type -AssemblyName System.Drawing
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
$files = @($Files | Where-Object { $_ -notlike "-*" })
if ($files.Count -eq 0) { Write-Host "contact_sheet: no input files given"; exit 1 }
$cw = 800; $ch = if ($Full) { 500 } else { 560 }   # -Full: the whole frame (tabs, title block, hints) at 16:10
$font = New-Object System.Drawing.Font "Arial", 12
$rows = [math]::Ceiling($files.Count / $Cols)
$bmp = New-Object System.Drawing.Bitmap ($cw * $Cols), ($ch * $rows)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
for ($i = 0; $i -lt $files.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile((Resolve-Path $files[$i]).Path)
  if ($Full) { $sx = 0; $sw = $img.Width; $sy = 0; $sh = $img.Height } else {
  $sx = [int]($img.Width * 0.16); $sw = [int]($img.Width * 0.68); $sy = [int]($img.Height * 0.06); $sh = [int]($sw * $ch / $cw) }
  $dst = New-Object System.Drawing.Rectangle (($i % $Cols) * $cw), ([math]::Floor($i / $Cols) * $ch), $cw, $ch
  $g.DrawImage($img, $dst, $sx, $sy, $sw, $sh, [System.Drawing.GraphicsUnit]::Pixel)
  $g.DrawString((Split-Path $files[$i] -Leaf), $font, [System.Drawing.Brushes]::White, $dst.X + 6, $dst.Y + 4)
  $img.Dispose()
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$font.Dispose(); $g.Dispose(); $bmp.Dispose()
