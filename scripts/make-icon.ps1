# Generates assets\minima.ico: a rounded-square gradient mark with a lowercase "m",
# matching the app's existing blue/slate gradient branding. Multi-resolution (16/32/48/256),
# each frame PNG-compressed inside the ICO container (supported since Windows Vista).
Add-Type -AssemblyName System.Drawing

function New-IconFrame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
    $radius = [Math]::Round($size * 0.22)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $c1 = [System.Drawing.Color]::FromArgb(255, 0x64, 0x74, 0x8b)
    $c2 = [System.Drawing.Color]::FromArgb(255, 0x25, 0x63, 0xeb)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 0.0)
    $g.FillPath($brush, $path)

    $fontSize = [float]($size * 0.56)
    $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $textBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $g.DrawString("m", $font, $textBrush, [float]($size/2), [float]($size/2 + $size*0.03), $fmt)

    $g.Dispose()
    return $bmp
}

$sizes = 16, 32, 48, 256
$frames = @()
foreach ($s in $sizes) { $frames += ,(New-IconFrame $s) }

$pngBlobs = @()
foreach ($bmp in $frames) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBlobs += ,$ms.ToArray()
}

$outPath = "D:\minima\assets\minima.ico"
$fs = [System.IO.File]::Open($outPath, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)

# ICONDIR
$bw.Write([UInt16]0)      # reserved
$bw.Write([UInt16]1)      # type = icon
$bw.Write([UInt16]$sizes.Count)

$headerSize = 6 + 16 * $sizes.Count
$offset = $headerSize
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $blob = $pngBlobs[$i]
    $wByte = if ($s -ge 256) { 0 } else { $s }
    $hByte = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$wByte)
    $bw.Write([byte]$hByte)
    $bw.Write([byte]0)    # color count
    $bw.Write([byte]0)    # reserved
    $bw.Write([UInt16]1)  # planes
    $bw.Write([UInt16]32) # bit count
    $bw.Write([UInt32]$blob.Length)
    $bw.Write([UInt32]$offset)
    $offset += $blob.Length
}
foreach ($blob in $pngBlobs) { $bw.Write($blob) }

$bw.Flush(); $bw.Close(); $fs.Close()
"Wrote $outPath"