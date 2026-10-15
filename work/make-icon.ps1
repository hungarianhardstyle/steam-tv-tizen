# Generates the Tizen TV app icon (512x512 PNG): rounded tile, gradient, gamepad glyph and the
# "STEAM TV" label. Samsung shows the icon in the TV's app list, so it must be a square PNG in the
# widget root, declared as <icon src="icon.png"/> in config.xml.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$size = 512
$output = Join-Path $PSScriptRoot 'steam-tv/icon.png'
$bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$graphics.Clear([System.Drawing.Color]::Transparent)

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# Tile with the Steam-ish blue gradient.
$tile = New-RoundedPath 8 8 ($size - 16) ($size - 16) 84
$brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.Point(0, 0)),
    (New-Object System.Drawing.Point($size, $size)),
    [System.Drawing.Color]::FromArgb(255, 23, 62, 96),
    [System.Drawing.Color]::FromArgb(255, 12, 22, 33))
$graphics.FillPath($brush, $tile)
$edge = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(70, 255, 255, 255), 4)
$graphics.DrawPath($edge, $tile)

# Glow behind the controller so the white glyph has something to sit on.
$glow = New-Object System.Drawing.Drawing2D.GraphicsPath
$glow.AddEllipse(96, 96, 320, 220)
$glowBrush = New-Object System.Drawing.Drawing2D.PathGradientBrush($glow)
$glowBrush.CenterColor = [System.Drawing.Color]::FromArgb(60, 120, 200, 240)
$glowBrush.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 120, 200, 240))
$graphics.FillPath($glowBrush, $glow)

# Gamepad: capsule body drawn as an outline with a filled D-pad and four face buttons inside - the
# arrangement reads as a controller at the app list's small tile size (two circles alone read as a
# face, and a filled silhouette turns into blobs once the details are punched out).
$white = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 18)
$white.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
$body = New-RoundedPath 116 152 280 144 70
$graphics.DrawPath($white, $body)
$solid = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
$graphics.FillRectangle($solid, 168, 213, 62, 22)
$graphics.FillRectangle($solid, 188, 193, 22, 62)
$graphics.FillEllipse($solid, 288, 192, 24, 24)
$graphics.FillEllipse($solid, 314, 218, 24, 24)
$graphics.FillEllipse($solid, 288, 244, 24, 24)
$graphics.FillEllipse($solid, 262, 218, 24, 24)
$graphics.FillRectangle($solid, 234, 200, 36, 14)

# "STEAM TV" label.
$font = New-Object System.Drawing.Font('Segoe UI', 46, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$format = New-Object System.Drawing.StringFormat
$format.Alignment = [System.Drawing.StringAlignment]::Center
$format.LineAlignment = [System.Drawing.StringAlignment]::Center
$graphics.DrawString('STEAM TV', $font, $solid, (New-Object System.Drawing.RectangleF(0, 366, $size, 90)), $format)

# Baseline tick marks at both ends of the label: a console-ish detail that reads well small.
$accent = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 143, 224, 200), 8)
$graphics.DrawLine($accent, 150, 448, 362, 448)

$graphics.Dispose()
$bitmap.Save($output, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Host "icon written: $output ($((Get-Item $output).Length) bytes)"
