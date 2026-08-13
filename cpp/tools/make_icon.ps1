# Generate app.ico (multi-resolution) for the PII sanitizer.
# Motif: teal rounded square + white shield + dark redaction bars (protect + mask).
# Also writes a 256px PNG preview. Pure System.Drawing (PowerShell 5.1).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$outDir = Split-Path $PSScriptRoot -Parent    # cpp/
$ico    = Join-Path $outDir 'src\app\app.ico'
$png    = Join-Path $outDir 'src\app\app_preview.png'

$teal   = [System.Drawing.Color]::FromArgb(255, 10, 125, 100)   # #0a7d64
$tealHi = [System.Drawing.Color]::FromArgb(255, 18, 150, 120)   # slightly lighter top
$white  = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)
$bar    = [System.Drawing.Color]::FromArgb(255, 40, 44, 48)     # redaction bar (near-black)

function New-RoundRectPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $r * 2
  $p.AddArc($x, $y, $d, $d, 180, 90)
  $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $p.CloseFigure()
  return $p
}

function Draw-Icon([int]$S) {
  $bmp = New-Object System.Drawing.Bitmap($S, $S)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.InterpolationMode = 'HighQualityBicubic'
  $g.Clear([System.Drawing.Color]::Transparent)

  # background: rounded square with a subtle vertical gradient
  $bg = New-RoundRectPath 0 0 $S $S ($S * 0.20)
  $rect = New-Object System.Drawing.RectangleF(0, 0, $S, $S)
  $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $tealHi, $teal, 90)
  $g.FillPath($grad, $bg)

  # shield (white)
  $sh = New-Object System.Drawing.Drawing2D.GraphicsPath
  $pts = @(
    (New-Object System.Drawing.PointF([single]($S*0.30), [single]($S*0.27))),
    (New-Object System.Drawing.PointF([single]($S*0.70), [single]($S*0.27))),
    (New-Object System.Drawing.PointF([single]($S*0.70), [single]($S*0.52))),
    (New-Object System.Drawing.PointF([single]($S*0.50), [single]($S*0.79))),
    (New-Object System.Drawing.PointF([single]($S*0.30), [single]($S*0.52)))
  )
  $sh.AddPolygon([System.Drawing.PointF[]]$pts)
  $sh.CloseFigure()
  $whiteBrush = New-Object System.Drawing.SolidBrush($white)
  $g.FillPath($whiteBrush, $sh)

  # redaction bars inside the shield (two lines)
  $barBrush = New-Object System.Drawing.SolidBrush($bar)
  foreach ($cy in 0.40, 0.505) {
    $bw = $S * 0.24
    $bx = $S * 0.38
    $by = $S * $cy
    $bh = [Math]::Max(2, $S * 0.05)
    $bp = New-RoundRectPath $bx $by $bw $bh ([Math]::Max(1, $bh * 0.4))
    $g.FillPath($barBrush, $bp)
  }

  $g.Dispose()
  return $bmp
}

# --- build .ico (PNG-compressed entries; Win10/11 handle PNG-in-ICO at all sizes) ---
$sizes = 256, 48, 32, 16
$blobs = @()
foreach ($s in $sizes) {
  $b = Draw-Icon $s
  $ms = New-Object System.IO.MemoryStream
  $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $blobs += , ($ms.ToArray())
  $ms.Dispose()
  if ($s -eq 256) { $b.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) }  # preview
  $b.Dispose()
}

$fs = [System.IO.File]::Open($ico, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)   # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $data = $blobs[$i]
  $dim = if ($s -ge 256) { 0 } else { $s }
  $bw.Write([byte]$dim); $bw.Write([byte]$dim)   # width, height (0 = 256)
  $bw.Write([byte]0); $bw.Write([byte]0)         # colorCount, reserved
  $bw.Write([uint16]1); $bw.Write([uint16]32)    # planes, bitCount
  $bw.Write([uint32]$data.Length); $bw.Write([uint32]$offset)
  $offset += $data.Length
}
foreach ($d in $blobs) { $bw.Write($d) }
$bw.Close(); $fs.Close()

Write-Host "wrote $ico ($((Get-Item $ico).Length) bytes)"
Write-Host "wrote $png (preview)"
