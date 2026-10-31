# Watch copy B's stand-in walk: A goes back to its start, B's player stands toward the cave
# with its camera turned to face A's start (once per B session: -Turn), then A walks for -Ms
# while B's window is captured -Count times; the crops become one strip.
# The walk: -Dx/-Dz, a world direction (A's stick is calibrated with walk_dir.py), or a raw
# stick -StickX/-StickY (camera-relative, so its world direction varies).
# Usage: view-standin.ps1 -Out <png prefix> [-Turn] [-Dx 1 -Dz 0 | -StickX -32767 -StickY 0]
#        [-Mag 1] [-Ms 1600] [-Count 6] [-EveryMs 60] [-DelayMs 500] [-X 420 -Y 180 -W 300 -H 230]
# Positions are the start of the test save in build 3095001 (NOTES.md, session ag).
param([string]$Out, [switch]$Turn, [double]$Dx = 0, [double]$Dz = 0, [double]$Mag = 1, [int]$StickX = -32767,
      [int]$StickY = 0, [int]$Ms = 1600, [int]$Count = 6, [int]$EveryMs = 60, [int]$DelayMs = 500, [int]$X = 420,
      [int]$Y = 180, [int]$W = 300, [int]$H = 230)
$T = $PSScriptRoot
$L = (Resolve-Path (Join-Path $T '..\..')).Path
$probe = "$T\probe.py"
$PA = "$L\test-installs\sow-a-profile"
$PB = "$L\test-installs\sow-b-profile"
$start = '23987.9 43092.5 12789.8'
if ($Turn) {
  python -I $probe $PB 'teleport 24000 43092.5 12420' | Out-Null
  Start-Sleep -Milliseconds 800
  python -I $probe $PB 'pad 0 0 0 32767 0 1200' | Out-Null
  Start-Sleep -Milliseconds 1400
}
if ($Dx -ne 0 -or $Dz -ne 0) {
  $cal = python -I "$T\walk_dir.py" $PA $start.Split(' ') $Dx $Dz 0 $Mag calibrate
  if ($cal -notmatch '^stick (-?\d+) (-?\d+)') { throw "calibration failed: $cal" }
  $StickX = [int]$Matches[1]; $StickY = [int]$Matches[2]
  "A's stick for world direction ${Dx},${Dz}: $StickX,$StickY"
} else {
  python -I $probe $PA "teleport $start" | Out-Null
}
Start-Sleep -Milliseconds 1000
python -I $probe $PA "pad 0 $StickX $StickY 0 0 $Ms" | Out-Null
Start-Sleep -Milliseconds $DelayMs
$shots = & "$T\shot.ps1" -Copy b -Out "$Out.png" -Count $Count -EveryMs $EveryMs -Width 960
$files = $shots | ForEach-Object { $_ -replace ' \d+x\d+ printwindow=.*$', '' }
& "$T\strip.ps1" -In $files -Out "$Out-strip.png" -X $X -Y $Y -W $W -H $H -Cols 3
python -I $probe $PB remote
