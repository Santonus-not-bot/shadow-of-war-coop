# Captures one test copy's game window into a ring of <Slots> PNG files in <Dir> every <EveryMs> ms for <TimeoutSec> s
# (the files' modification times say when). Usage: ring_shot.ps1 -Copy a -Dir <dir> [-Slots 240 -EveryMs 200 -Width 480 -TimeoutSec 1800]
param([string]$Copy = 'a', [string]$Dir, [int]$Slots = 240, [int]$EveryMs = 200, [int]$Width = 480, [int]$TimeoutSec = 1800)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RingNative {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
$L = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
$end = (Get-Date).AddSeconds($TimeoutSec); $i = 0
while ((Get-Date) -lt $end) {
  $p = Get-Process ShadowOfWar -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith("$L\test-installs\sow-$Copy\", 'OrdinalIgnoreCase') } | Select-Object -First 1
  if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
    $h = $p.MainWindowHandle; $r = New-Object RingNative+RECT
    [void][RingNative]::GetClientRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -gt 0 -and $ht -gt 0) {
      $bmp = New-Object System.Drawing.Bitmap $w, $ht
      $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
      [void][RingNative]::PrintWindow($h, $dc, 3)
      $g.ReleaseHdc($dc); $g.Dispose()
      $sw = [Math]::Min($Width, $w); $sh = [int]($ht * $sw / $w)
      $small = New-Object System.Drawing.Bitmap $bmp, $sw, $sh
      $small.Save((Join-Path $Dir ("r{0:d3}.png" -f ($i % $Slots))), [System.Drawing.Imaging.ImageFormat]::Png)
      $small.Dispose(); $bmp.Dispose(); $i++
    }
  }
  Start-Sleep -Milliseconds $EveryMs
}
