# Wait until a test copy's guard log reports the world starting (or giving up), then print the input lines.
param([string]$Copy, [int]$TimeoutSec = 150)
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path  # the local/ folder
$log = "$Root\test-installs\sow-$Copy-profile\sowguard.log"
$start = Get-Date
Start-Sleep -Seconds 5
do {
  Start-Sleep -Seconds 3
  $lines = Get-Content $log
  $i = ($lines | Select-String -Pattern 'sowguard start' | Select-Object -Last 1).LineNumber
  $sess = $lines[($i - 1)..($lines.Count - 1)]
  $done = $sess | Select-String -Pattern 'world starting|no world start reported|virtual pad off|FATAL'
} while (-not $done -and ((Get-Date) - $start).TotalSeconds -lt $TimeoutSec)
"sow-${Copy}: " + $(if ($done) { $done[-1].Line } else { 'no result in time' })
