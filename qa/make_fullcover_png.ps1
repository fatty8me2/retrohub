# Makes a synthetic full cover scan (BACK | SPINE | FRONT laid flat, left to right) for the cases shots: HUB_SHOT_FULLCOVER=<png>.
# The spine text is rotated 90 degrees clockwise (reads top to bottom), exactly like a real flat cover sheet.
# Usage: qa\make_fullcover_png.ps1 -Out qa_out\cases-faces\fullcover_ds.png -FrontAspect 1.12 [-Height 900]
param([string]$Out, [double]$FrontAspect = 0.71, [int]$Height = 900)
Add-Type -AssemblyName System.Drawing
$fw = [int]($Height * $FrontAspect); $spine = [int]($fw * 0.1); if ($FrontAspect -gt 1) { $spine = [int]($fw * 0.07) }
$W = $fw * 2 + $spine
$bmp = New-Object System.Drawing.Bitmap $W, $Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAliasGridFit'
function Brush($c) { New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($c)) }
function Font($s, $st = 'Bold') { New-Object System.Drawing.Font 'Segoe UI', $s, ([System.Drawing.FontStyle]::$st), 'Pixel' }
# back
$g.FillRectangle((New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Rectangle]::new(0, 0, $fw, $Height)), ([System.Drawing.ColorTranslator]::FromHtml('#1f6f4a')), ([System.Drawing.ColorTranslator]::FromHtml('#0c2a1c')), 90), 0, 0, $fw, $Height)
$g.DrawString('SYNTHETIC BACK', (Font ($Height * 0.06)), (Brush '#ffffff'), ($fw * 0.06), ($Height * 0.05))
for ($i = 0; $i -lt 3; $i++) { $g.FillRectangle((Brush '#e8b84a'), ($fw * 0.06 + $i * $fw * 0.31), ($Height * 0.3), ($fw * 0.27), ($Height * 0.2)); $g.DrawString("shot $($i+1)", (Font ($Height * 0.04)), (Brush '#222222'), ($fw * 0.08 + $i * $fw * 0.31), ($Height * 0.37)) }
for ($i = 0; $i -lt 9; $i++) { $g.FillRectangle((Brush '#cfe8d8'), ($fw * 0.06), ($Height * (0.56 + $i * 0.032)), ($fw * (0.88 - ($i % 3) * 0.12)), ($Height * 0.012)) }
$g.FillRectangle((Brush '#ffffff'), ($fw * 0.66), ($Height * 0.9), ($fw * 0.28), ($Height * 0.07))
$g.DrawString('back | edge', (Font ($Height * 0.03) 'Regular'), (Brush '#ffffff'), ($fw * 0.06), ($Height * 0.93))
# spine (text rotated 90 degrees clockwise: reads top to bottom)
$g.FillRectangle((Brush '#c8102e'), $fw, 0, $spine, $Height)
$g.FillRectangle((Brush '#ffffff'), $fw, 0, $spine, ($Height * 0.1))
$st = $g.Save()
$g.TranslateTransform(($fw + $spine), 0); $g.RotateTransform(90)
$g.DrawString('SYNTHETIC SPINE TEXT READS TOP TO BOTTOM', (Font ($spine * 0.5)), (Brush '#ffffff'), ($Height * 0.14), ($spine * 0.12))
$g.Restore($st)
# front
$g.FillRectangle((New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Rectangle]::new(($fw + $spine), 0, $fw, $Height)), ([System.Drawing.ColorTranslator]::FromHtml('#2a64d6')), ([System.Drawing.ColorTranslator]::FromHtml('#0a1c4a')), 90), ($fw + $spine), 0, $fw, $Height)
$g.FillRectangle((Brush '#f2f3f6'), ($fw + $spine), 0, $fw, ($Height * 0.08))
$g.DrawString('SYNTHETIC CONSOLE BANNER', (Font ($Height * 0.045)), (Brush '#222222'), ($fw + $spine + $fw * 0.04), ($Height * 0.015))
$g.DrawString('FRONT', (Font ($Height * 0.2)), (Brush '#ffd23f'), ($fw + $spine + $fw * 0.08), ($Height * 0.3))
$g.DrawString('full cover test', (Font ($Height * 0.06) 'Regular'), (Brush '#ffffff'), ($fw + $spine + $fw * 0.1), ($Height * 0.62))
$g.Dispose()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$bmp.Save((Join-Path (Get-Location) $Out), [System.Drawing.Imaging.ImageFormat]::Png)
"wrote $Out ${W}x$Height (front $fw, spine $spine)"
