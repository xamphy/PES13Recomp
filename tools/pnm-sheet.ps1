# Contact sheet of PPM (P6) / PGM (P5) files - renderer surface dumps and their .alpha.pgm -
# each scaled by -Scale, labelled with the file name.
#   tools\pnm-sheet.ps1 -Out sheet.png -Scale 2 a.ppm a.ppm.alpha.pgm b.ppm ...
param([Parameter(Mandatory)][string]$Out, [int]$Scale = 2, [int]$Cols = 4,
      [Parameter(ValueFromRemainingArguments)][string[]]$Files)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
function Load([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $txt = [Text.Encoding]::ASCII.GetString($b, 0, [Math]::Min(32, $b.Length))
    $hdr = $txt -split '\s+'; $kind = $hdr[0]; $w = [int]$hdr[1]; $h = [int]$hdr[2]
    $off = $txt.IndexOf("255`n") + 4
    $ch = if ($kind -eq 'P6') { 3 } else { 1 }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) {
        $i = $off + ($y * $w + $x) * $ch
        $c = if ($ch -eq 3) { [System.Drawing.Color]::FromArgb($b[$i], $b[$i + 1], $b[$i + 2]) } else { [System.Drawing.Color]::FromArgb($b[$i], $b[$i], $b[$i]) }
        $bmp.SetPixel($x, $y, $c) } }
    return $bmp
}
$imgs = foreach ($f in $Files) { [pscustomobject]@{ Name = [IO.Path]::GetFileName($f); Bmp = Load (Resolve-Path $f).Path } }
$cellW = ($imgs | ForEach-Object { $_.Bmp.Width } | Measure-Object -Maximum).Maximum * $Scale
$cellH = ($imgs | ForEach-Object { $_.Bmp.Height } | Measure-Object -Maximum).Maximum * $Scale + 18
$rows = [Math]::Ceiling($imgs.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ([int]($cellW * [Math]::Min($Cols, $imgs.Count))), ([int]($cellH * $rows))
$g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::FromArgb(255, 60, 0, 60))
$g.InterpolationMode = 'NearestNeighbor'; $font = New-Object System.Drawing.Font 'Arial', 9
for ($i = 0; $i -lt $imgs.Count; $i++) {
    $x = ($i % $Cols) * $cellW; $y = [Math]::Floor($i / $Cols) * $cellH
    $g.DrawString($imgs[$i].Name, $font, [System.Drawing.Brushes]::White, $x + 2, $y + 2)
    $g.DrawImage($imgs[$i].Bmp, $x, $y + 18, $imgs[$i].Bmp.Width * $Scale, $imgs[$i].Bmp.Height * $Scale)
}
$sheet.Save([IO.Path]::GetFullPath($Out), [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $sheet.Dispose()
Write-Output $Out
