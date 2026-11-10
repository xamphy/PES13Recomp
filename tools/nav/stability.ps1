# Stability A/B: alternates match sessions with write tracking off / on and reports crashes.
param([int]$Pairs = 2, [int]$PlaySeconds = 120, [switch]$AllOn)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$c = Join-Path $root 'tools\nav\ctl.ps1'
for ($i = 1; $i -le $Pairs * 2; $i++) {
    $track = $AllOn -or ($i % 2 -eq 0)
    $tag = if ($track) { "on$i" } else { "off$i" }
    if (-not $track) { $env:PES_VB_HASH = '1'; $env:PES_TEX_HASH = '1' }
    Stop-Process -Name pes13 -Force -ErrorAction SilentlyContinue; Start-Sleep 2
    $log = "$env:TEMP\pes_stab_$tag.log"
    $env:PES_AUDIO_MUTE = '1'
    & (Join-Path $root 'tools\nav\run-nav.ps1') -Control -Log $log | Out-Null
    Remove-Item Env:PES_AUDIO_MUTE, Env:PES_VB_HASH, Env:PES_TEX_HASH -ErrorAction SilentlyContinue
    $ok = $true
    try {
        Start-Sleep 95
        foreach ($s in @(@('press start',4),@('press start',4),@('press a',9),@('press a',4),@('press a',5),@('press a',6),@('press a',10),@('press a',4),@('press a',8),@('press a',6),@('press a',30))) {
            & $c $s[0] -Wait $s[1] | Out-Null
        }
        foreach ($k in 1..30) { & $c 'press a' -Wait 4 | Out-Null }
        $end = (Get-Date).AddSeconds($PlaySeconds)
        while ((Get-Date) -lt $end) { if (-not (Get-Process pes13 -ErrorAction SilentlyContinue)) { break }; Start-Sleep 5 }
    } catch { $ok = $false }
    $alive = [bool](Get-Process pes13 -ErrorAction SilentlyContinue)
    Stop-Process -Name pes13 -Force -ErrorAction SilentlyContinue
    $crash = (Get-Content ($log -replace '\.log$', '.err.log') | Where-Object { $_ -match '\[crash\]' }) -join ' '
    $frames = (Get-Content $log | Where-Object { $_ -match '^\[gpu\] \d+ fps, (\d+) frames' } | Select-Object -Last 1) -replace '^\[gpu\] \d+ fps, (\d+) frames.*', '$1'
    "{0}: tracking {1} | alive at end {2} | frames {3} | {4}" -f $tag, $track, $alive, $frames, $crash
}
