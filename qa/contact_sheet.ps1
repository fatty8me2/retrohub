# Makes a contact sheet (PNG) of several screenshots: crops the middle of each (where the centre case is) and tiles them in a grid.
# Usage: qa\contact_sheet.ps1 out.png a.png b.png c.png d.png [-Cols 2] [-Cell 800x560]
param([Parameter(Mandatory = $true, Position = 0)][string]$Out, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Files, [int]$Cols = 2)
Add-Type -AssemblyName System.Drawing
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
$files = @($Files | Where-Object { $_ -notlike "-*" })
$cw = 800; $ch = 560
$rows = [math]::Ceiling($files.Count / $Cols)
$bmp = New-Object System.Drawing.Bitmap ($cw * $Cols), ($ch * $rows)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
for ($i = 0; $i -lt $files.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile((Resolve-Path $files[$i]).Path)
  $sx = [int]($img.Width * 0.16); $sw = [int]($img.Width * 0.68); $sy = [int]($img.Height * 0.06); $sh = [int]($sw * $ch / $cw)
  $dst = New-Object System.Drawing.Rectangle (($i % $Cols) * $cw), ([math]::Floor($i / $Cols) * $ch), $cw, $ch
  $g.DrawImage($img, $dst, $sx, $sy, $sw, $sh, [System.Drawing.GraphicsUnit]::Pixel)
  $g.DrawString((Split-Path $files[$i] -Leaf), (New-Object System.Drawing.Font "Arial", 12), [System.Drawing.Brushes]::White, $dst.X + 6, $dst.Y + 4)
  $img.Dispose()
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
