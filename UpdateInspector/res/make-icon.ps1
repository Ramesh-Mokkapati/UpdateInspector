# Regenerates UpdateInspector.ico: a blue tile with a white "download into tray" arrow
# and a green check badge. Every size is drawn separately so small sizes stay crisp.
#   powershell -ExecutionPolicy Bypass -File UpdateInspector\res\make-icon.ps1   (from the solution folder)
param([string]$Out = (Join-Path $PSScriptRoot 'UpdateInspector.ico'), [string]$PreviewDir)

Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function New-RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $path
}

function Draw-Icon([int]$size) {
    $s = [float]$size
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    # Tile
    $inset = [Math]::Max(0.5, 0.04 * $s)
    $tile = New-RoundedRect $inset $inset ($s - 2 * $inset) ($s - 2 * $inset) (0.2 * $s)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF 0, $s), ([System.Drawing.Color]::FromArgb(255, 58, 140, 240)), ([System.Drawing.Color]::FromArgb(255, 22, 78, 170))
    $g.FillPath($brush, $tile)

    $white = [System.Drawing.Color]::White
    $stroke = [Math]::Max(1.5, 0.085 * $s)

    # Tray (open box the arrow drops into)
    $pen = New-Object System.Drawing.Pen $white, $stroke
    $pen.LineJoin = 'Round'; $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $tray = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF (0.19 * $s), (0.56 * $s)),
        (New-Object System.Drawing.PointF (0.19 * $s), (0.78 * $s)),
        (New-Object System.Drawing.PointF (0.63 * $s), (0.78 * $s)),
        (New-Object System.Drawing.PointF (0.63 * $s), (0.56 * $s)))
    $g.DrawLines($pen, $tray)

    # Down arrow
    $cx = 0.41 * $s
    $shaft = [Math]::Max(1.5, 0.15 * $s) / 2
    $head = 0.2 * $s
    $arrow = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($cx - $shaft), (0.16 * $s)),
        (New-Object System.Drawing.PointF ($cx + $shaft), (0.16 * $s)),
        (New-Object System.Drawing.PointF ($cx + $shaft), (0.42 * $s)),
        (New-Object System.Drawing.PointF ($cx + $head),  (0.42 * $s)),
        (New-Object System.Drawing.PointF $cx,            (0.66 * $s)),
        (New-Object System.Drawing.PointF ($cx - $head),  (0.42 * $s)),
        (New-Object System.Drawing.PointF ($cx - $shaft), (0.42 * $s)))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush $white), $arrow)

    # Green check badge, bottom right, with a white ring to separate it from the tile
    $r = 0.21 * $s
    $bx = 0.72 * $s; $by = 0.72 * $s
    $ring = [Math]::Max(1.0, 0.045 * $s)
    $g.FillEllipse((New-Object System.Drawing.SolidBrush $white), ($bx - $r - $ring), ($by - $r - $ring), (2 * ($r + $ring)), (2 * ($r + $ring)))
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 34, 160, 90))), ($bx - $r), ($by - $r), (2 * $r), (2 * $r))
    $check = New-Object System.Drawing.Pen $white, ([Math]::Max(1.4, 0.06 * $s))
    $check.LineJoin = 'Round'; $check.StartCap = 'Round'; $check.EndCap = 'Round'
    $g.DrawLines($check, [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($bx - 0.46 * $r), ($by + 0.02 * $r)),
        (New-Object System.Drawing.PointF ($bx - 0.12 * $r), ($by + 0.33 * $r)),
        (New-Object System.Drawing.PointF ($bx + 0.48 * $r), ($by - 0.31 * $r))))

    $g.Dispose()
    $bmp
}

# ICO file: header, one directory entry per size, then PNG-compressed images.
$images = foreach ($size in $sizes) {
    $bmp = Draw-Icon $size
    if ($PreviewDir) { $bmp.Save((Join-Path $PreviewDir "icon-$size.png")) }
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

$file = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $file
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[System.IO.File]::WriteAllBytes($Out, $file.ToArray())
"Wrote $Out ($($file.Length) bytes, sizes: $($sizes -join ', '))"
