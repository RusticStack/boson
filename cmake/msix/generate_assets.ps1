# Generates placeholder MSIX logos (a rounded tile with the app's initial) when the app ships none.
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Color = '#3B6FE0'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$initial = if ($Name.Length -gt 0) { $Name.Substring(0, 1).ToUpperInvariant() } else { 'B' }
$brushColor = [System.Drawing.ColorTranslator]::FromHtml($Color)
$sizes = @{ 'Square44x44Logo.png' = 44; 'Square150x150Logo.png' = 150; 'StoreLogo.png' = 50 }
foreach ($entry in $sizes.GetEnumerator()) {
    $size = $entry.Value
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $g.Clear([System.Drawing.Color]::Transparent)
        $radius = [Math]::Max(4, [int]($size / 5))
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = $radius * 2
        $path.AddArc(0, 0, $d, $d, 180, 90)
        $path.AddArc($size - $d - 1, 0, $d, $d, 270, 90)
        $path.AddArc($size - $d - 1, $size - $d - 1, $d, $d, 0, 90)
        $path.AddArc(0, $size - $d - 1, $d, $d, 90, 90)
        $path.CloseFigure()
        $brush = New-Object System.Drawing.SolidBrush $brushColor
        $g.FillPath($brush, $path)
        $font = New-Object System.Drawing.Font 'Segoe UI', ([single]($size * 0.5)), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
        $format = New-Object System.Drawing.StringFormat
        $format.Alignment = [System.Drawing.StringAlignment]::Center
        $format.LineAlignment = [System.Drawing.StringAlignment]::Center
        $rect = New-Object System.Drawing.RectangleF 0, 0, $size, $size
        $g.DrawString($initial, $font, [System.Drawing.Brushes]::White, $rect, $format)
        $bmp.Save((Join-Path $OutDir $entry.Key), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $g.Dispose()
        $bmp.Dispose()
    }
}
