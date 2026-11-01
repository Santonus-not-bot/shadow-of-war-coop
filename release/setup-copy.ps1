# Makes a separate copy of Middle-earth: Shadow of War for co-op and puts the mod in it.
#
# The mod never runs in your Steam install: it needs its own copy of the game folder (about 110 GB; the game's files
# are copied, not moved, and your Steam install, your saves and your Steam cloud are only read, never written).
# In the copy the mod redirects the game's save folders and Steam cloud to a folder of its own (<CopyDir>\profile), so
# a co-op session cannot touch your real saves.
#
# Usage (PowerShell):
#   .\setup-copy.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Middle-earth Shadow of War" `
#                    -CopyDir "E:\SoWCoop" -ModDll .\bink2w64.dll -Role host -SaveFile "<your save>"
#   .\setup-copy.ps1 -GameDir ... -CopyDir ... -ModDll .\bink2w64.dll -Role client -HostAddress 100.64.0.5:47011
# Add -DryRun to see what it would do without doing it.
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$GameDir,   # the Steam install (the folder that contains x64\ShadowOfWar.exe)
  [Parameter(Mandatory)] [string]$CopyDir,   # where the copy goes; must not be inside a Steam folder
  [Parameter(Mandatory)] [string]$ModDll,    # the mod's build of bink2w64.dll
  [ValidateSet('host', 'client')] [string]$Role = 'host',
  [string]$HostAddress = '',                 # client: <host address>:<port> (a VPN address, see README-PLAYERS.md)
  [int]$Port = 47011,                        # host: UDP port
  [string]$Password = '',                    # both: the same word on both sides (optional)
  [string]$SaveFile = '',                    # host: your save to play (<id>_ShadowOfWar.sav from the game's save/cloud folder)
  [switch]$SkipExeCheck,                     # tests only
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$knownSha = 'ed0d95cabb033cd66aaa99d699374881846c3cb03ec345f1123d82d45e0bdaef'  # Steam build 3095001
function Step($m) { Write-Host "==> $m" }
function Act($m, [scriptblock]$b) { if ($DryRun) { Write-Host "    (dry run) $m" } else { Write-Host "    $m"; & $b } }

$GameDir = (Resolve-Path $GameDir).Path
$exe = Join-Path $GameDir 'x64\ShadowOfWar.exe'
if (-not (Test-Path $exe)) { throw "$exe not found: -GameDir must be the folder that contains x64\ShadowOfWar.exe" }
if (-not (Test-Path $ModDll)) { throw "$ModDll not found" }
$ModDll = (Resolve-Path $ModDll).Path
$CopyFull = [IO.Path]::GetFullPath($CopyDir)
$inSteam = $CopyFull -like '*\Steam\*' -or $CopyFull -like '*\SteamLibrary\*' -or $CopyFull -like '*\steamapps\*'
if ($inSteam -or $CopyFull.StartsWith($GameDir, 'OrdinalIgnoreCase') -or $GameDir.StartsWith($CopyFull, 'OrdinalIgnoreCase')) {
  throw "-CopyDir must be a folder of its own, outside the Steam folders and outside the game folder (got $CopyFull)"
}
if ($Role -eq 'client' -and -not $HostAddress) { throw 'a client needs -HostAddress <host address>:<port>' }
if ($Role -eq 'client' -and $HostAddress -notmatch '^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$') { throw '-HostAddress must look like 100.64.0.5:47011 (an IPv4 address and a port)' }

Step 'checking the game'
if (-not $SkipExeCheck) {
  $sha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
  if ($sha -ne $knownSha) { throw "ShadowOfWar.exe is not the build this mod is made for (Steam build 3095001). SHA-256 $sha" }
  Write-Host '    ShadowOfWar.exe is the expected build'
}
Step 'checking the disk'
$need = (Get-ChildItem $GameDir -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
$drive = (Split-Path $CopyFull -Qualifier)
$free = (Get-PSDrive ($drive.TrimEnd(':'))).Free
Write-Host ("    the game is {0:N1} GB, {1} has {2:N1} GB free" -f ($need / 1GB), $drive, ($free / 1GB))
if ($need -gt $free -and -not $DryRun) { throw "not enough free space on $drive" }

Step "copying the game to $CopyFull (this takes a while)"
Act 'robocopy (the original is only read)' {
  New-Item -ItemType Directory -Force $CopyFull | Out-Null
  robocopy $GameDir $CopyFull /E /R:1 /W:1 /NFL /NDL /NP /XF 'steam_appid.txt' | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
}

$x64 = Join-Path $CopyFull 'x64'
$profile = Join-Path $CopyFull 'profile'
Step 'putting the mod in'
Act 'the original bink2w64.dll becomes bink2w64_.dll; the mod takes its place' {
  if (-not (Test-Path (Join-Path $x64 'bink2w64_.dll'))) { Move-Item (Join-Path $x64 'bink2w64.dll') (Join-Path $x64 'bink2w64_.dll') }
  Copy-Item $ModDll (Join-Path $x64 'bink2w64.dll') -Force
  Set-Content -Path (Join-Path $x64 'steam_appid.txt') -Value '356190' -Encoding ASCII
}
Act 'a profile folder for the redirected saves and the Steam cloud' {
  New-Item -ItemType Directory -Force (Join-Path $profile 'steam-userdata\356190\remote') | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $profile 'LocalAppData\WB Games\Shadow of War') | Out-Null
}
if ($Role -eq 'host' -and $SaveFile) {
  Act "your save goes into the copy's cloud folder" {
    $name = Split-Path $SaveFile -Leaf
    Copy-Item $SaveFile (Join-Path $profile "steam-userdata\356190\remote\$name") -Force
    Copy-Item $SaveFile (Join-Path $profile "LocalAppData\WB Games\Shadow of War\$name") -Force
  }
} elseif ($Role -eq 'host') {
  Write-Host '    no -SaveFile: the host starts the game, saves once in it, then restarts it (clients get the newest save)'
}

Step 'writing sowguard.ini'
$ini = @(
  '[sowguard]', 'test_mode=0',
  "allowed_root=$CopyFull", "profile=$profile",
  'network=coop_peer', 'steam_cloud=sandbox',
  'probe=1', 'probe_write=0', 'hook_debug_print=1', 'debug_channels=2A34D9E',
  'autoload=1', 'autoload_first_ms=20000', 'autoload_every_ms=8000', 'autoload_max_presses=0',
  'background_run=1', 'sim_hook=1F96818:36',
  "coop_role=$Role",
  'coop_show=puppet', 'coop_puppet_ai=off', 'coop_puppet_move=anim', 'coop_puppet_spawn=Talion',
  'coop_enemies=on')
if ($Role -eq 'host') { $ini += "coop_port=$Port"; $ini += 'coop_decoy=on' } else { $ini += "coop_host=$HostAddress" }
if ($Password) { $ini += "coop_password=$Password" }
Act (Join-Path $x64 'sowguard.ini') { Set-Content -Path (Join-Path $x64 'sowguard.ini') -Value $ini -Encoding ASCII }

Step 'done'
Write-Host "    start it with:  .\launch.ps1 -CopyDir '$CopyFull'"
Write-Host '    the mod writes its log to <CopyDir>\profile\sowguard.log'
if ($Role -eq 'host') { Write-Host "    the log lists the addresses a friend can use: lines 'a client on this network or VPN joins with coop_host=...'" }
