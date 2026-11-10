# Benchmark: boots muted, walks into an exhibition match (GS vs FB), skips the cutscenes and lets
# live play run; prints the renderer / fps stats logged during the last $PlaySeconds of play.
param([string]$Log = "$env:TEMP\pes_bench.log", [int]$PlaySeconds = 60, [string]$Tag = 'bench', [int]$SkipPresses = 16)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$c = Join-Path $root 'tools\nav\ctl.ps1'
$env:PES_AUDIO_MUTE = '1'
& (Join-Path $root 'tools\nav\run-nav.ps1') -Control -Log $Log
Remove-Item Env:PES_AUDIO_MUTE
Start-Sleep 95
foreach ($s in @(@('press start',4),@('press start',4),@('press a',9),@('press a',4),@('press a',5),@('press a',6),@('press a',10),@('press a',4),@('press a',8),@('press a',6),@('press a',30))) {
    & $c $s[0] -Wait $s[1] | Out-Null
}
# Skip the cutscenes until live play (screenshot brightness check is unreliable; count presses), then hands off.
foreach ($i in 1..$SkipPresses) { & $c 'press a' -Wait 4 | Out-Null }
Start-Sleep 10
Start-Sleep 5
& $c "shot ${Tag}_start" | Out-Null
$mark = (Get-Content $Log).Count
$cpu0 = (Get-Process pes13).TotalProcessorTime.TotalSeconds; $t0 = Get-Date
Start-Sleep $PlaySeconds
$cpu1 = (Get-Process pes13).TotalProcessorTime.TotalSeconds; $wall = ((Get-Date) - $t0).TotalSeconds
"[bench] $Tag process CPU during play: {0:N2} cores busy ({1:N0} s CPU in {2:N0} s)" -f (($cpu1 - $cpu0) / $wall), ($cpu1 - $cpu0), $wall
& $c "shot ${Tag}_end" | Out-Null
Stop-Process -Name pes13 -Force -ErrorAction SilentlyContinue
Wait-Process -Name pes13 -Timeout 30 -ErrorAction SilentlyContinue
Get-Content $Log | Select-Object -Skip $mark | Where-Object { $_ -match '^\[gpu\] \d+ fps|last 120 frames|GPU|per-draw|descriptor sets|tiles moved' }
