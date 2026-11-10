# Resolve a crash offset (rva printed by "[crash] ... at rva 0x...", or the
# "Fault offset" from the Windows event log) to the nearest symbol in pes13.map.
#   powershell -ExecutionPolicy Bypass -File tools\addr2func.ps1 0x212C929
param(
    [Parameter(Mandatory)][string]$Rva,
    [string]$Map
)
$ErrorActionPreference = 'Stop'
if (-not $Map) { $Map = Join-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) '..\build\Release\bin\pes13.map' }
if (-not (Test-Path $Map)) { throw "map file not found: $Map (rebuild with tools\build-runtime.bat)" }

$imageBase = [Convert]::ToUInt64('140000000', 16)
$target = $imageBase + [Convert]::ToUInt64(($Rva -replace '^0x', ''), 16)

$best = $null; $bestAddr = [uint64]0
foreach ($line in [IO.File]::ReadLines((Resolve-Path $Map))) {
    # MSVC-style map rows: " 0001:00009950       <symbol>   000000014000a950   <lib:obj>"
    $m = [regex]::Match($line, '^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\b')
    if (-not $m.Success) { continue }
    $name = $m.Groups[1].Value
    $addr = [Convert]::ToUInt64($m.Groups[2].Value, 16)
    if ($addr -le $target -and $addr -gt $bestAddr -or ($addr -eq $bestAddr -and $name -like '__imp__sub_*')) {
        $best = $name; $bestAddr = $addr
    }
}
if (-not $best) { "no symbol found for $Rva"; exit 1 }
"{0} + 0x{1:X}" -f $best, ($target - $bestAddr)
if ($best -match 'sub_([0-9A-F]{8})') { "guest function: 0x$($Matches[1])  (generated code: Select-String -Path generated\*.cpp -Pattern 'PPC_FUNC_IMPL\(__imp__sub_$($Matches[1])\)')" }

