Add-Type -AssemblyName System.Drawing

$uiDir = $PSScriptRoot
$pngPath = Join-Path $uiDir "app_icon.png"
$icoPath = Join-Path $uiDir "app.ico"

function HsvToColor([double]$H, [double]$S, [double]$V) {
    $hi = [int][Math]::Floor($H / 60.0) % 6
    $f = ($H / 60.0) - [Math]::Floor($H / 60.0)
    $p = $V * (1.0 - $S)
    $q = $V * (1.0 - $f * $S)
    $t = $V * (1.0 - (1.0 - $f) * $S)
    switch ($hi) {
        0 { $r = $V; $g = $t; $b = $p }
        1 { $r = $q; $g = $V; $b = $p }
        2 { $r = $p; $g = $V; $b = $t }
        3 { $r = $p; $g = $q; $b = $V }
        4 { $r = $t; $g = $p; $b = $V }
        default { $r = $V; $g = $p; $b = $q }
    }
    return [System.Drawing.Color]::FromArgb(255, [int]($r * 255), [int]($g * 255), [int]($b * 255))
}

$size = 256
$bmp = New-Object System.Drawing.Bitmap $size, $size
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.Clear([System.Drawing.Color]::Transparent)

# 1. Rounded square background card
$pad = 12
$rect = New-Object System.Drawing.Rectangle $pad, $pad, ($size - 2 * $pad), ($size - 2 * $pad)
$rad = 44
$bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
$bgPath.AddArc($rect.X, $rect.Y, $rad, $rad, 180, 90)
$bgPath.AddArc($rect.Right - $rad, $rect.Y, $rad, $rad, 270, 90)
$bgPath.AddArc($rect.Right - $rad, $rect.Bottom - $rad, $rad, $rad, 0, 90)
$bgPath.AddArc($rect.X, $rect.Bottom - $rad, $rad, $rad, 90, 90)
$bgPath.CloseFigure()

$bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.Point 0, 0),
    (New-Object System.Drawing.Point 0, $size),
    ([System.Drawing.Color]::FromArgb(255, 245, 249, 255)),
    ([System.Drawing.Color]::FromArgb(255, 224, 234, 246))
)
$g.FillPath($bgBrush, $bgPath)
$borderPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 176, 198, 222)), 4
$g.DrawPath($borderPen, $bgPath)

# 2. Top: Isometric 3D CATIA Part model (stepped block + boss)
# Helper to convert isometric (u, v, z) to screen (x, y)
$ox = 128.0
$oy = 96.0
$scale = 3.1
function IsoPt([double]$u, [double]$v, [double]$z) {
    $sx = $ox + ($u - $v) * 0.866 * $scale
    $sy = $oy + ($u + $v) * 0.5 * $scale - $z * $scale
    return New-Object System.Drawing.PointF ([float]$sx), ([float]$sy)
}

$edgePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 22, 54, 88)), 2.8
$edgePen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

$topBrush   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 130, 192, 246))
$leftBrush  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 58, 128, 196))
$rightBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 36, 92, 152))
$hiBrush    = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 174, 218, 255))

# Base plate: u in [-18..18], v in [-14..14], z in [0..8]
$p_top = @((IsoPt -18 -14 8), (IsoPt 18 -14 8), (IsoPt 18 14 8), (IsoPt -18 14 8))
$p_left = @((IsoPt -18 14 8), (IsoPt 18 14 8), (IsoPt 18 14 0), (IsoPt -18 14 0))
$p_right = @((IsoPt 18 -14 8), (IsoPt 18 14 8), (IsoPt 18 14 0), (IsoPt 18 -14 0))

$g.FillPolygon($leftBrush, $p_left)
$g.DrawPolygon($edgePen, $p_left)
$g.FillPolygon($rightBrush, $p_right)
$g.DrawPolygon($edgePen, $p_right)
$g.FillPolygon($topBrush, $p_top)
$g.DrawPolygon($edgePen, $p_top)

# Upper stepped rib/block on back-left: u in [-18..-2], v in [-14..14], z in [8..19]
$u_top = @((IsoPt -18 -14 19), (IsoPt -2 -14 19), (IsoPt -2 14 19), (IsoPt -18 14 19))
$u_left = @((IsoPt -18 14 19), (IsoPt -2 14 19), (IsoPt -2 14 8), (IsoPt -18 14 8))
$u_right = @((IsoPt -2 -14 19), (IsoPt -2 14 19), (IsoPt -2 14 8), (IsoPt -2 -14 8))

$g.FillPolygon($leftBrush, $u_left)
$g.DrawPolygon($edgePen, $u_left)
$g.FillPolygon($rightBrush, $u_right)
$g.DrawPolygon($edgePen, $u_right)
$g.FillPolygon($hiBrush, $u_top)
$g.DrawPolygon($edgePen, $u_top)

# Small cylindrical boss on front-right deck: center (8, 0, 8)
$bossRect = New-Object System.Drawing.RectangleF ([float](128 + 8 * 0.866 * $scale - 14)), ([float](96 + 8 * 0.5 * $scale - 8 * $scale - 13)), 28, 17
$bossSide = New-Object System.Drawing.RectangleF $bossRect.X, ($bossRect.Y + 6), 28, 14
$g.FillEllipse($rightBrush, $bossSide)
$g.FillRectangle($leftBrush, $bossRect.X, ($bossRect.Y + 8), 28, 6)
$g.FillEllipse($hiBrush, $bossRect)
$g.DrawEllipse($edgePen, $bossRect)

# 3. Bottom: Vibrant Color Wheel
$cx = 128.0
$cy = 186.0
$outerR = 46.0
$innerR = 18.0

$steps = 120
for ($i = 0; $i -lt $steps; $i++) {
    $a1 = ($i * 360.0 / $steps)
    $sweep = (360.0 / $steps) + 1.2
    $c = HsvToColor $a1 0.90 0.95
    $br = New-Object System.Drawing.SolidBrush $c
    $g.FillPie($br, [float]($cx - $outerR), [float]($cy - $outerR), [float](2 * $outerR), [float](2 * $outerR), [float]$a1, [float]$sweep)
    $br.Dispose()
}

# Outer ring border + inner white hub
$wheelBorderPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 42, 62, 84)), 2.5
$g.DrawEllipse($wheelBorderPen, [float]($cx - $outerR), [float]($cy - $outerR), [float](2 * $outerR), [float](2 * $outerR))

$hubBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 244, 248, 253))
$g.FillEllipse($hubBrush, [float]($cx - $innerR), [float]($cy - $innerR), [float](2 * $innerR), [float](2 * $innerR))
$g.DrawEllipse($wheelBorderPen, [float]($cx - $innerR), [float]($cy - $innerR), [float](2 * $innerR), [float](2 * $innerR))

$g.Dispose()
$bmp.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

# Convert PNG to multi-resolution ICO
. "C:\BOM-gemini\CATPart_BOM_Tool_V4\Source\UI\convert_icon.ps1"
Convert-PngToIco -PngPath $pngPath -IcoPath $icoPath
