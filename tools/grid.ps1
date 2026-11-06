# Puts frames <prefix>-NN.png (the numbers given) into one grid image, 3 columns, each scaled to 480x270.
# Usage: grid.ps1 -Prefix <path without -NN.png> -Frames 1,3,5 -Out <png>
param([string]$Prefix, [int[]]$Frames, [string]$Out)
Add-Type -AssemblyName System.Drawing
$w = 480; $h = 270; $cols = 3
$rows = [math]::Ceiling($Frames.Count / $cols)
$bmp = New-Object System.Drawing.Bitmap ($w * $cols), ($h * $rows)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$font = New-Object System.Drawing.Font('Arial', 14)
for ($i = 0; $i -lt $Frames.Count; $i++) {
  $f = '{0}-{1:D2}.png' -f $Prefix, $Frames[$i]
  if (-not (Test-Path $f)) { continue }
  $img = [System.Drawing.Image]::FromFile($f)
  $x = ($i % $cols) * $w; $y = [math]::Floor($i / $cols) * $h
  $g.DrawImage($img, $x, $y, $w, $h)
  $g.DrawString(('{0:D2}' -f $Frames[$i]), $font, [System.Drawing.Brushes]::Yellow, $x + 4, $y + 4)
  $img.Dispose()
}
$bmp.Save($Out)
$g.Dispose(); $bmp.Dispose()
"saved $Out"
