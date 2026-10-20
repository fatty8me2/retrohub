Add-Type -AssemblyName System.Drawing
Get-ChildItem $PSScriptRoot -Filter *.bmp | ForEach-Object {
  $b = [System.Drawing.Image]::FromFile($_.FullName); $b.Save(($_.FullName -replace '\.bmp$','.png'), [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); Remove-Item $_.FullName }
