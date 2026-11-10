# Sends commands to a running pes13 started with PES_CONTROL=userdata\control.txt and waits
# until each is acknowledged (see renderer.cpp, "PES_CONTROL"). Examples:
#   tools\nav\ctl.ps1 'press a'
#   tools\nav\ctl.ps1 'press down' 'press down' 'shot menu1'
#   tools\nav\ctl.ps1 'capture match1'          # waits until the capture is written
#   tools\nav\ctl.ps1 -Wait 3 'press a'         # then sleeps 3 s (screen transition)
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(Position = 0, ValueFromRemainingArguments)][string[]]$Commands, [double]$Wait = 0, [int]$Timeout = 60)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$control = Join-Path $root 'userdata\control.txt'
$ack = "$control.ack"
if (-not (Get-Process pes13 -ErrorAction SilentlyContinue)) { throw 'pes13.exe is not running.' }
function AckLines { if (Test-Path -LiteralPath $ack) { ,[string[]](Get-Content -LiteralPath $ack) } else { ,[string[]]@() } }
foreach ($c in $Commands) {
    $before = (AckLines).Count
    $expect = if ($c -match '^capture\s+(\S+)') { 'done capture ' + $Matches[1] } else { $null }
    [System.IO.File]::AppendAllText($control, "$c`n")
    $deadline = (Get-Date).AddSeconds($Timeout)
    while ($true) {
        $lines = AckLines
        $new = if ($lines.Count -gt $before) { $lines[$before..($lines.Count - 1)] } else { @() }
        $err = $new | Where-Object { $_ -like 'error*' }
        if ($err) { throw "pes13 rejected: $err" }
        if (($expect -and ($new -contains $expect)) -or (-not $expect -and $new.Count -gt 0)) {
            Write-Output (@($new) -join ' | ')
            # Shots: convert to PNG next to the PPM (for viewing).
            if ($c -match '^shot\s+(\S+)') { & (Join-Path $root 'tools\ppm2png.ps1') (Join-Path $root "userdata\shots\$($Matches[1]).ppm") -Width 960 | Out-Null }
            break
        }
        if ((Get-Date) -gt $deadline) { throw "no acknowledgement for '$c' within $Timeout s" }
        Start-Sleep -Milliseconds 100
    }
}
if ($Wait -gt 0) { Start-Sleep -Milliseconds ([int]($Wait * 1000)) }
