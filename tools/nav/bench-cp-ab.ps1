# Same-scene A/B of a runtime switch: walks into an exhibition match like bench-match.ps1, then
#   -Cmd cp  (command processor waits, sleep|spin)      -Cmd d3d (D3D wait loops, sleep|spin)
#   -Cmd sync -A targeted -B broadcast                  (kernel wait wakeups)
# alternates control "<Cmd> <A>" / "<Cmd> <B>" every $WindowSeconds and reports fps + process CPU
# (cores busy) per mode, so goals/replays/set pieces hit both modes alike.
param([string]$Cmd = 'cp', [string]$A = 'sleep', [string]$B = 'spin', [string]$Log ="$env:TEMP\pes_bench_cpab.log", [int]$Cycles = 8, [int]$WindowSeconds = 15, [int]$SkipPresses = 16)
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
foreach ($i in 1..$SkipPresses) { & $c 'press a' -Wait 4 | Out-Null }
Start-Sleep 15
$res = @{ $A = @{ fps = @(); cpu = 0.0; wall = 0.0 }; $B = @{ fps = @(); cpu = 0.0; wall = 0.0 } }
for ($i = 0; $i -lt $Cycles * 2; $i++) {
    $mode = if ($i % 2 -eq 0) { $A } else { $B }
    & $c "$Cmd $mode" | Out-Null
    Start-Sleep 2   # settle; not measured
    $mark = (Get-Content $Log).Count
    $cpu0 = (Get-Process pes13).TotalProcessorTime.TotalSeconds; $t0 = Get-Date
    Start-Sleep $WindowSeconds
    $cpu1 = (Get-Process pes13).TotalProcessorTime.TotalSeconds; $wall = ((Get-Date) - $t0).TotalSeconds
    $fps = Get-Content $Log | Select-Object -Skip $mark | ForEach-Object { if ($_ -match '^\[gpu\] (\d+) fps') { [int]$Matches[1] } }
    $res[$mode].fps += $fps; $res[$mode].cpu += $cpu1 - $cpu0; $res[$mode].wall += $wall
    "{0,-9} window {1,2}: fps avg {2,5:N1} | cores busy {3:N2}" -f $mode, $i, ($fps | Measure-Object -Average).Average, (($cpu1 - $cpu0) / $wall)
}
& $c 'shot cpab_end' | Out-Null
Stop-Process -Name pes13 -Force -ErrorAction SilentlyContinue
Wait-Process -Name pes13 -Timeout 30 -ErrorAction SilentlyContinue
foreach ($m in $A, $B) {
    $f = $res[$m].fps | Measure-Object -Average -Minimum -Maximum
    "[ab] {0,-9}: fps avg {1:N1} (min {2}, max {3}, {4} samples) | cores busy {5:N2}" -f $m, $f.Average, $f.Minimum, $f.Maximum, $f.Count, ($res[$m].cpu / $res[$m].wall)
}
