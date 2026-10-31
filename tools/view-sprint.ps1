# Watch copy B's stand-in (or A's own player, -Copy a) while A runs and then free-runs (A
# held): A goes back to its start, runs along a world direction for -RunMs, then holds A for
# -SprintMs while the chosen copy's window is captured -Count times; the crops become one strip.
# Run view-standin.ps1 -Turn once per B session first, so B's camera faces A's start.
# Usage: view-sprint.ps1 -Out <png prefix> [-Copy b] [-Dx 1 -Dz 0] [-RunMs 800] [-SprintMs 900]
#        [-Count 6] [-EveryMs 60] [-DelayMs 400] [-X 420 -Y 180 -W 300 -H 230]
param([string]$Out, [string]$Copy = 'b', [double]$Dx = 1, [double]$Dz = 0, [int]$RunMs = 800,
      [int]$SprintMs = 900, [int]$Count = 6, [int]$EveryMs = 60, [int]$DelayMs = 400, [int]$X = 420,
      [int]$Y = 180, [int]$W = 300, [int]$H = 230)
$T = $PSScriptRoot
$L = (Resolve-Path (Join-Path $T '..\..')).Path
$probe = "$T\probe.py"
$PA = "$L\test-installs\sow-a-profile"
$PB = "$L\test-installs\sow-b-profile"
$start = '23987.9 43092.5 12789.8'
$cal = python -I "$T\walk_dir.py" $PA $start.Split(' ') $Dx $Dz 0 1 calibrate
if ($cal -notmatch '^stick (-?\d+) (-?\d+)') { throw "calibration failed: $cal" }
$sx = [int]$Matches[1]; $sy = [int]$Matches[2]
python -I $probe $PB 'puppet snap 5000' | Out-Null
Start-Sleep -Milliseconds 1500
python -I $probe $PA "pad 0 $sx $sy 0 0 $RunMs" | Out-Null
Start-Sleep -Milliseconds ($RunMs - 50)
python -I $probe $PA "pad 1000 $sx $sy 0 0 $SprintMs" | Out-Null
Start-Sleep -Milliseconds $DelayMs
$shots = & "$T\shot.ps1" -Copy $Copy -Out "$Out.png" -Count $Count -EveryMs $EveryMs -Width 960
$files = $shots | ForEach-Object { $_ -replace ' \d+x\d+ printwindow=.*$', '' }
& "$T\strip.ps1" -In $files -Out "$Out-strip.png" -X $X -Y $Y -W $W -H $H -Cols 3
Start-Sleep -Milliseconds 600
python -I $probe $PB 'puppet snap 150' | Out-Null
python -I $probe $PB remote
