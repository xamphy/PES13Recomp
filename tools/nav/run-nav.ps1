# Runs pes13 for tests: an optional timed navigation script from tools/nav and/or live control.
# Examples:
#   tools\nav\run-nav.ps1 -Script shot-drill -Shots '141,144,147' -Log "$env:TEMP\pes_x.log"
#   tools\nav\run-nav.ps1 -Control -Log "$env:TEMP\pes_x.log"    # then drive it with tools\nav\ctl.ps1
# Extra PES_* variables: set them in the calling shell before running.
param([string]$Script = '', [switch]$Control, [string]$Shots = '', [string]$Log = "$env:TEMP\pes_nav.log")
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (Get-Process pes13 -ErrorAction SilentlyContinue) { throw 'pes13.exe is already running.' }
if ($Script) { $env:PES_INPUT_SCRIPT = (Get-Content -LiteralPath (Join-Path $PSScriptRoot "$Script.txt") -Raw).Trim() }
else { Remove-Item Env:PES_INPUT_SCRIPT -ErrorAction SilentlyContinue }
if ($Control) {
    # Live control (tools\nav\ctl.ps1): start with an empty command file.
    $cf = Join-Path $root 'userdata\control.txt'
    Set-Content -LiteralPath $cf -Value $null
    Remove-Item -LiteralPath "$cf.ack" -ErrorAction SilentlyContinue
    $env:PES_CONTROL = $cf
}
else { Remove-Item Env:PES_CONTROL -ErrorAction SilentlyContinue }
if ($Shots) { $env:PES_SHOTS = $Shots } else { Remove-Item Env:PES_SHOTS -ErrorAction SilentlyContinue }
# Hidden, never Minimized: a minimized window is not rendered (no frames, no shots).
$p = Start-Process -FilePath (Join-Path $root 'build\Release\bin\pes13.exe') -ArgumentList '--run' -WorkingDirectory $root `
    -WindowStyle Hidden -RedirectStandardOutput $Log -RedirectStandardError ($Log -replace '\.log$', '.err.log') -PassThru
Write-Output "PID $($p.Id); script '$Script'; control $Control; log $Log"
