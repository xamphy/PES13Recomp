# Starts pes13 under live control and walks into an exhibition match with the default teams
# (Guinea vs Saudi Arabia on a fresh profile with saves). Steps verified 2026-11-07 with
# screenshots c01-c17. Leaves the game running at kick-off; screenshots along the way are
# userdata\shots\ex_*.png. Afterwards drive it with tools\nav\ctl.ps1 (shot / capture / press).
#   tools\nav\exhibition.ps1 [-Log path]
param([string]$Log = "$env:TEMP\pes_ex.log", [switch]$StopBeforeCutscenes, [switch]$StopAtPrematch)
$ErrorActionPreference = 'Stop'
$ctl = Join-Path $PSScriptRoot 'ctl.ps1'
& (Join-Path $PSScriptRoot 'run-nav.ps1') -Control -Log $Log
function Step([string]$cmd, [double]$wait) { & $ctl $cmd -Wait $wait | Out-Null }

Start-Sleep 95                       # boot + Konami logo + intro movie start
Step 'press start' 4                 # skip intro movie -> title "PRESS START"
Step 'press start' 4                 # title -> "Select Storage Device"
Step 'shot ex_storage' 0
Step 'press a' 9                     # storage -> top menu (MATCH selected)
Step 'shot ex_topmenu' 0
Step 'press a' 4                     # MATCH -> Exhibition Match
Step 'press a' 5                     # Exhibition -> side selection
Step 'press a' 5                     # sides -> team selection (home region)
Step 'press a' 3                     # home region -> home team
Step 'press a' 3                     # home team -> away region
Step 'press a' 4                     # away region/team -> pre-match screen
Step 'shot ex_prematch' 0
if ($StopAtPrematch) { Write-Output 'Stopped at the pre-match screen (KICK-OFF selected).'; return }
Step 'press a' 22                    # KICK-OFF -> loading -> tunnel
if ($StopBeforeCutscenes) { Step 'shot ex_tunnel' 0; Write-Output 'Stopped before the pre-match cutscenes.'; return }
foreach ($i in 1..45) { Step 'press a' 4 }     # tunnel, ceremony, line-ups, formations (~3 min; A in play = a pass)
Start-Sleep 12
Step 'shot ex_kickoff' 0
Write-Output 'At kick-off (check userdata\shots\ex_kickoff.png).'
