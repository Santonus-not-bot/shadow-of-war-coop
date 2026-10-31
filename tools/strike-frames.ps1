# One strike seen from both test copies: B's player at the watch spot (live_suite.watch_spot), B's
# probe given -BCmd first (e.g. "puppet moves on"), then A strikes while both game windows are
# captured (PrintWindow, game windows only); writes a contact sheet of A's player (top rows) and
# B's stand-in (bottom rows), cropped, and deletes the frames.
# Usage: strike-frames.ps1 -Out <png> [-BCmd "<probe command>"] [-Count 12] [-EveryMs 150]
param([string]$Out, [string]$BCmd = '', [int]$Count = 12, [int]$EveryMs = 150)
$L = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path  # the local/ folder
$T = Join-Path $L 'sow-coop\tools'
$dir = Split-Path $Out
$stem = [IO.Path]::GetFileNameWithoutExtension($Out)
@"
import sys
sys.path.insert(0, r'$T')
from live_suite import Copy, settle, watch_spot
a, b = Copy('a'), Copy('b')
if r'''$BCmd''':
    print('B <-', r'''$BCmd''', ':', b.ask(r'''$BCmd''').strip())
settle(a, b)
watch_spot(a, b)
"@ | python -I -
$jobs = foreach ($c in 'a', 'b') {
  Start-Job -ScriptBlock { param($sh, $copy, $o, $n, $ms) & $sh -Copy $copy -Out $o -Count $n -EveryMs $ms } `
    -ArgumentList (Join-Path $T 'shot.ps1'), $c, (Join-Path $dir "$stem-$c.png"), $Count, $EveryMs
}
Start-Sleep -Milliseconds 1700
& python -I (Join-Path $T 'probe.py') (Join-Path $L 'test-installs\sow-a-profile') 'pad 4000 0 0 0 0 150' | Out-Null
Wait-Job $jobs -Timeout 40 | Out-Null
Add-Type -AssemblyName System.Drawing
[int]$cols = [math]::Ceiling($Count / 2); [int]$cw = 300; [int]$ch = 330
$sheet = New-Object System.Drawing.Bitmap ([int]($cw * $cols)), ([int]($ch * 4))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$font = New-Object System.Drawing.Font 'Arial', 14
foreach ($c in 'a', 'b') {
  # A's view: its player left of centre; B's view: the stand-in beside the pillar.
  $src = if ($c -eq 'a') { New-Object System.Drawing.Rectangle 280, 90, 360, 400 } else { New-Object System.Drawing.Rectangle 360, 110, 360, 400 }
  for ($i = 0; $i -lt $Count; $i++) {
    $f = Join-Path $dir ("$stem-$c-{0:D2}.png" -f $i)
    if (-not (Test-Path $f)) { continue }
    $row = [math]::Floor($i / $cols) * 2 + ($(if ($c -eq 'a') { 0 } else { 1 }))
    $img = [System.Drawing.Image]::FromFile($f)
    $g.DrawImage($img, (New-Object System.Drawing.Rectangle (($i % $cols) * $cw), ($row * $ch), $cw, $ch), $src, [System.Drawing.GraphicsUnit]::Pixel)
    $img.Dispose()
    $g.DrawString("$($c.ToUpper()) $i", $font, [System.Drawing.Brushes]::Yellow, (($i % $cols) * $cw + 4), ($row * $ch + 4))
    Remove-Item $f
  }
}
$g.Dispose(); $sheet.Save($Out); $sheet.Dispose()
"sheet $Out"
