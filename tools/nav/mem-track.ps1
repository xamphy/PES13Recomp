# Memory over a session: boots, walks into an exhibition match like bench-match.ps1, lets play run for
# $PlayMinutes and prints the renderer's [mem] report (process memory + Vulkan allocations by kind)
# about once a minute, from boot to the end. Set PES_NO_EVICT=1 / PES_UPLOAD_MB=<n> beforehand for A/B.
param([string]$Log = "$env:TEMP\pes_mem.log", [int]$PlayMinutes = 6, [int]$SkipPresses = 30, [string]$Tag = 'mem')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$c = Join-Path $root 'tools\nav\ctl.ps1'
$env:PES_AUDIO_MUTE = '1'
& (Join-Path $root 'tools\nav\run-nav.ps1') -Control -Log $Log | Out-Null
Remove-Item Env:PES_AUDIO_MUTE
$t0 = Get-Date
Start-Sleep 95
$step = 0
foreach ($s in @(@('press start',4),@('press start',4),@('press a',9),@('press a',4),@('press a',5),@('press a',6),@('press a',10),@('press a',4),@('press a',8),@('press a',6),@('press a',30))) {
    & $c $s[0] -Wait $s[1] | Out-Null
    if (++$step -in 3, 7, 10) { & $c "shot ${Tag}_menu$step" | Out-Null }
}
foreach ($i in 1..$SkipPresses) { & $c 'press a' -Wait 4 | Out-Null }
& $c "shot ${Tag}_play" | Out-Null
$end = (Get-Date).AddMinutes($PlayMinutes)
while ((Get-Date) -lt $end -and (Get-Process pes13 -ErrorAction SilentlyContinue)) { Start-Sleep 10 }
$alive = [bool](Get-Process pes13 -ErrorAction SilentlyContinue)
if ($alive) { & $c "shot ${Tag}_end" | Out-Null }
Stop-Process -Name pes13 -Force -ErrorAction SilentlyContinue
Wait-Process -Name pes13 -Timeout 30 -ErrorAction SilentlyContinue

# [mem] lines come in pairs every 120 frames (~2-4 s); keep about one pair per minute plus the last.
$all = Get-Content $Log
$lines = $all | Where-Object { $_ -match '^\[mem\] (process|images)' }
$pairs = [math]::Floor($lines.Count / 2)
$step = [math]::Max(1, [math]::Floor($pairs / [math]::Max(1, (((Get-Date) - $t0).TotalMinutes))))
for ($p = 0; $p -lt $pairs; $p += $step) { $lines[2 * $p]; $lines[2 * $p + 1] }
if ($pairs -gt 0 -and ($pairs - 1) % $step -ne 0) { '-- last:'; $lines[2 * ($pairs - 1)]; $lines[2 * ($pairs - 1) + 1] }
$all | Where-Object { $_ -match '^\[mem\] vertex pages' } | Select-Object -Last 1
$all | Where-Object { $_ -match '^\[mem\] surfaces used' } | Select-Object -Last 1
$all | Where-Object { $_ -match 'pool overflowed' }
# Last render-target table (printed every 10th report).
$start = -1
for ($i = $all.Count - 1; $i -ge 0; $i--) { if ($all[$i] -match '^\[mem\] render targets') { $start = $i; break } }
if ($start -ge 0) { $all[$start]; for ($i = $start + 1; $i -lt $all.Count -and $all[$i] -match '^\[mem\]   '; $i++) { $all[$i] } }
"[$Tag] alive at end: $alive | crash: " + ((Get-Content ($Log -replace '\.log$', '.err.log') | Where-Object { $_ -match '\[crash\]' }) -join ' ')
