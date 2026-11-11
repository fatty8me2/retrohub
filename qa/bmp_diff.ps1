# Pixel-compares two BMP directories: prints identical / differing pixel count and bounding box per file.
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B)
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$A\*.bmp" | ForEach-Object {
  $n = $_.Name
  $ha = (Get-FileHash $_.FullName).Hash; $hb = (Get-FileHash "$B\$n").Hash
  if ($ha -eq $hb) { "$n IDENTICAL (byte-for-byte)"; return }
  $x = [System.Drawing.Bitmap]::FromFile($_.FullName); $y = [System.Drawing.Bitmap]::FromFile("$B\$n")
  $cnt = 0; $x0 = 99999; $y0 = 99999; $x1 = -1; $y1 = -1
  for ($j = 0; $j -lt $x.Height; $j++) { for ($i = 0; $i -lt $x.Width; $i++) {
    if ($x.GetPixel($i, $j).ToArgb() -ne $y.GetPixel($i, $j).ToArgb()) { $cnt++; if ($i -lt $x0) { $x0 = $i }; if ($i -gt $x1) { $x1 = $i }; if ($j -lt $y0) { $y0 = $j }; if ($j -gt $y1) { $y1 = $j } } } }
  $x.Dispose(); $y.Dispose()
  "$n DIFF pixels=$cnt bbox=($x0,$y0)-($x1,$y1)"
}
