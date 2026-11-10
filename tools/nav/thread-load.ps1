# Per-thread CPU during live play: walks into an exhibition match like bench-match.ps1, samples every
# host thread's CPU time over $Seconds, and maps the busiest ones to guest threads (control "threads":
# name, host id, guest stack) or known host threads (CP thread id from the log).
param([string]$Log = "$env:TEMP\pes_threadload.log", [int]$Seconds = 20, [int]$SkipPresses = 16, [int]$Top = 12)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$c = Join-Path $root 'tools\nav\ctl.ps1'
$env:PES_AUDIO_MUTE = '1'
& (Join-Path $root 'tools\nav\run-nav.ps1') -Control -Log $Log | Out-Null
Remove-Item Env:PES_AUDIO_MUTE
Start-Sleep 95
foreach ($s in @(@('press start',4),@('press start',4),@('press a',9),@('press a',4),@('press a',5),@('press a',6),@('press a',10),@('press a',4),@('press a',8),@('press a',6),@('press a',30))) {
    & $c $s[0] -Wait $s[1] | Out-Null
}
foreach ($i in 1..$SkipPresses) { & $c 'press a' -Wait 4 | Out-Null }
Start-Sleep 15

function Snap { $h = @{}; foreach ($t in (Get-Process pes13).Threads) { try { $h[$t.Id] = $t.TotalProcessorTime.TotalSeconds } catch {} }; $h }
$a = Snap; $t0 = Get-Date
Start-Sleep ([int]($Seconds / 2))
$mark = (Get-Content $Log).Count
& $c 'threads' | Out-Null
Start-Sleep ($Seconds - [int]($Seconds / 2))
$b = Snap; $wall = ((Get-Date) - $t0).TotalSeconds
& $c 'shot threadload' | Out-Null
Start-Sleep 2
$dump = Get-Content $Log | Select-Object -Skip $mark
Stop-Process -Name pes13 -Force -ErrorAction SilentlyContinue
Wait-Process -Name pes13 -Timeout 30 -ErrorAction SilentlyContinue

# host id -> guest thread line + following stack line
$guest = @{}
for ($i = 0; $i -lt $dump.Count; $i++) {
    if ($dump[$i] -match '^\s+(\d+)\s+(.+?)\s+\[host\s+(\d+)\]') {
        $gid = $Matches[1]; $name = $Matches[2].Trim(); $hid = [int]$Matches[3]
        $stack = if ($i + 1 -lt $dump.Count -and $dump[$i + 1] -like '*guest stack*') { $dump[$i + 1].Trim() } else { '' }
        $guest[$hid] = "guest $gid $name | $stack"
    }
}
$cp = (Get-Content $Log | Where-Object { $_ -match 'CP.*host thread (id )?(\d+)' } | Select-Object -First 1)
$cpId = if ($cp -match '(\d+)\s*$') { [int]$Matches[1] } else { -1 }

$rows = foreach ($id in $b.Keys) { $d = $b[$id] - $(if ($a.ContainsKey($id)) { $a[$id] } else { 0 }); [pscustomobject]@{ Id = $id; Cores = $d / $wall } }
$total = ($rows | Measure-Object Cores -Sum).Sum
"[threads] total {0:N2} cores busy over {1:N0} s" -f $total, $wall
foreach ($r in ($rows | Sort-Object Cores -Descending | Select-Object -First $Top)) {
    $what = if ($guest.ContainsKey($r.Id)) { $guest[$r.Id] } elseif ($r.Id -eq $cpId) { 'host: command processor' } else { 'host thread (not a running guest thread)' }
    "  {0,6}  {1,5:N2}  {2}" -f $r.Id, $r.Cores, $what
}
