# Capture copy B's stand-in while A is steered along a world direction (steer.py runs in the
# background from A's start), then crop the captures into one strip. B's camera should face
# A's start already (view-standin.ps1 -Turn once per B session).
# Usage: view-steer.ps1 -Out <png prefix> [-Dx -1 -Dz 0] [-Secs 1.8] [-Count 9] [-EveryMs 50]
#        [-DelayMs 600] [-X 200 -Y 150 -W 420 -H 300]
param([string]$Out, [double]$Dx = -1, [double]$Dz = 0, [double]$Secs = 1.8, [int]$Count = 9, [int]$EveryMs = 50,
      [int]$DelayMs = 600, [int]$X = 200, [int]$Y = 150, [int]$W = 420, [int]$H = 300)
$T = $PSScriptRoot
$L = (Resolve-Path (Join-Path $T '..\..')).Path
$PA = "$L\test-installs\sow-a-profile"
$PB = "$L\test-installs\sow-b-profile"
python -I "$T\probe.py" $PA 'teleport 23987.9 43092.5 12789.8' | Out-Null
Start-Sleep -Milliseconds 1200
$job = Start-Process -FilePath python -ArgumentList @('-I', "`"$T\steer.py`"", "`"$PA`"", 'here', $Dx, $Dz, $Secs) `
  -NoNewWindow -PassThru -RedirectStandardOutput "$Out-steer.txt"
Start-Sleep -Milliseconds $DelayMs
$shots = & "$T\shot.ps1" -Copy b -Out "$Out.png" -Count $Count -EveryMs $EveryMs -Width 960
$job.WaitForExit(15000) | Out-Null
Get-Content "$Out-steer.txt"
$files = $shots | ForEach-Object { $_ -replace ' \d+x\d+ printwindow=.*$', '' }
& "$T\strip.ps1" -In $files -Out "$Out-strip.png" -X $X -Y $Y -W $W -H $H -Cols 3
python -I "$T\probe.py" $PB remote
