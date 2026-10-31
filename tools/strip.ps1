# Crop the same rectangle out of several captures and lay the crops side by side (row-major),
# to compare frames of a movement in one image.
# Usage: strip.ps1 -In <png> <png> ... -Out <png> -X 380 -Y 170 -W 340 -H 250 [-Cols 3]
param([string[]]$In, [string]$Out, [int]$X, [int]$Y, [int]$W, [int]$H, [int]$Cols = 3)
Add-Type -AssemblyName System.Drawing
$rows = [Math]::Ceiling($In.Count / $Cols)
$strip = New-Object System.Drawing.Bitmap ($W * $Cols), ($H * $rows)
$g = [System.Drawing.Graphics]::FromImage($strip)
for ($i = 0; $i -lt $In.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile($In[$i])
  $dst = New-Object System.Drawing.Rectangle (($i % $Cols) * $W), ([Math]::Floor($i / $Cols) * $H), $W, $H
  $src = New-Object System.Drawing.Rectangle $X, $Y, $W, $H
  $g.DrawImage($img, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
  $img.Dispose()
}
$g.Dispose()
$strip.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$strip.Dispose()
"$Out $($W * $Cols)x$($H * $rows)"
