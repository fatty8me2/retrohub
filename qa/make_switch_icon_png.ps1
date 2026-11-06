# Makes a synthetic square Switch game icon (256x256) for the cases shots: HUB_SHOT_ICON=<png>.
param([string]$Out = "qa_out\cases-faces\icon_switch.png")
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 256, 256
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAliasGridFit'
$g.FillRectangle((New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Rectangle]::new(0, 0, 256, 256)), ([System.Drawing.ColorTranslator]::FromHtml('#35c0e8')), ([System.Drawing.ColorTranslator]::FromHtml('#1c3fb0')), 60), 0, 0, 256, 256)
$g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#ffd23f'))), 70, 40, 116, 116)
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#f2f3f6'))), 20, 170, 216, 60)
$f = New-Object System.Drawing.Font 'Segoe UI', 34, ([System.Drawing.FontStyle]::Bold), 'Pixel'
$g.DrawString('ICON', $f, (New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#c8102e'))), 62, 178)
$g.Dispose()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$bmp.Save((Join-Path (Get-Location) $Out), [System.Drawing.Imaging.ImageFormat]::Png)
"wrote $Out"
