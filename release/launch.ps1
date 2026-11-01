# Starts the co-op copy made by setup-copy.ps1 and says where its log is.
param([Parameter(Mandatory)] [string]$CopyDir)
$x64 = Join-Path ([IO.Path]::GetFullPath($CopyDir)) 'x64'
$exe = Join-Path $x64 'ShadowOfWar.exe'
if (-not (Test-Path (Join-Path $x64 'sowguard.ini'))) { throw "$x64\sowguard.ini not found: run setup-copy.ps1 first" }
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { Write-Warning 'Steam is not running; the game needs it running (and you logged in).' }
Write-Host "starting $exe ; log: $(Join-Path $CopyDir 'profile\sowguard.log')"
Start-Process -FilePath $exe -WorkingDirectory $x64
