# Capture one test copy's game window (PrintWindow, so other windows on top do not matter)
# to a PNG. Usage: shot.ps1 -Copy b -Out <png> [-Count n -EveryMs ms]
param([string]$Copy = 'b', [string]$Out, [int]$Count = 1, [int]$EveryMs = 250, [int]$Width = 960)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ShotNative {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
$L = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path  # the local/ folder
$p = Get-Process ShadowOfWar -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith("$L\test-installs\sow-$Copy\", 'OrdinalIgnoreCase') } | Select-Object -First 1
if (-not $p -or $p.MainWindowHandle -eq [IntPtr]::Zero) { throw "sow-$Copy has no window" }
$h = $p.MainWindowHandle
$r = New-Object ShotNative+RECT
[void][ShotNative]::GetClientRect($h, [ref]$r)
$w = $r.R - $r.L; $ht = $r.B - $r.T
for ($i = 0; $i -lt $Count; $i++) {
  $bmp = New-Object System.Drawing.Bitmap $w, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  $ok = [ShotNative]::PrintWindow($h, $dc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
  $g.ReleaseHdc($dc); $g.Dispose()
  $sw = [Math]::Min($Width, $w); $sh = [int]($ht * $sw / $w)
  $small = New-Object System.Drawing.Bitmap $bmp, $sw, $sh
  $file = if ($Count -gt 1) { $Out -replace '\.png$', ("-{0:d2}.png" -f $i) } else { $Out }
  $small.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
  $small.Dispose(); $bmp.Dispose()
  "$file ${w}x$ht printwindow=$ok"
  if ($i -lt $Count - 1) { Start-Sleep -Milliseconds $EveryMs }
}
