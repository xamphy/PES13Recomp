# Prints the PowerPC disassembly of a recompiled function (from the comments in the
# generated code), plus its direct callers.
#   powershell -ExecutionPolicy Bypass -File tools\show-func.ps1 822F2178 [-MaxLines 200]
param([Parameter(Mandatory)][string]$Address, [int]$MaxLines = 250, [switch]$NoCallers)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent
$name = 'sub_' + ($Address -replace '^0x', '').ToUpper()

$hit = Select-String -Path "$root\generated\ppc_recomp.*.cpp" -Pattern "PPC_FUNC_IMPL(__imp__$name)" -SimpleMatch | Select-Object -First 1
if (-not $hit) { Write-Host "$name not found"; exit 1 }

$printed = 0
foreach ($line in (Get-Content $hit.Path | Select-Object -Skip $hit.LineNumber)) {
    if ($line.StartsWith('PPC_FUNC_IMPL(') -or $line.StartsWith('__attribute__')) { break }
    if ($line -match '^\t// (.*)$') { "  " + $Matches[1] }
    elseif ($line -match '^(loc_[0-9A-F]+):') { $Matches[1] + ':' }
    elseif ($line -match '^\t(?:return )?(sub_[0-9A-F]{8}|__imp__\w+)\(ctx, base\);') { "      -> " + $Matches[1] }
    elseif ($line -match 'PPC_CALL_INDIRECT_FUNC') { "      -> (indirect)" }
    else { continue }
    if (++$printed -ge $MaxLines) { "  ... (truncated)"; break }
}

if (-not $NoCallers) {
    $callers = Select-String -Path "$root\generated\ppc_recomp.*.cpp" -Pattern "^\t(return )?$name\(ctx, base\);" | ForEach-Object {
        $lines = Get-Content $_.Path -TotalCount $_.LineNumber
        ($lines | Select-String 'PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)' | Select-Object -Last 1).Matches[0].Groups[1].Value
    } | Sort-Object -Unique
    "callers: " + ($callers -join ' ')
}
