# Converts binary PPM (P6) files - renderer screenshots/dumps - to PNG, optionally downscaled.
#   powershell -ExecutionPolicy Bypass -File tools\ppm2png.ps1 userdata\shots [-Width 640]
param([Parameter(Mandatory)][string]$Path, [int]$Width = 0)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$files = if (Test-Path $Path -PathType Container) { Get-ChildItem $Path -Filter *.ppm } else { Get-Item $Path }
foreach ($f in $files) {
    $b = [IO.File]::ReadAllBytes($f.FullName)
    $txt = [Text.Encoding]::ASCII.GetString($b, 0, [Math]::Min(32, $b.Length))
    $hdr = $txt -split '\s+'; $w = [int]$hdr[1]; $h = [int]$hdr[2]
    $off = $txt.IndexOf("255`n") + 4
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h), 'WriteOnly', $bmp.PixelFormat)
    $row = New-Object byte[] ($data.Stride)
    for ($y = 0; $y -lt $h; $y++) {
        $src = $off + $y * $w * 3
        for ($x = 0; $x -lt $w; $x++) { $i = $src + $x * 3; $row[$x * 3] = $b[$i + 2]; $row[$x * 3 + 1] = $b[$i + 1]; $row[$x * 3 + 2] = $b[$i] }
        [Runtime.InteropServices.Marshal]::Copy($row, 0, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $data.Stride)
    }
    $bmp.UnlockBits($data)
    if ($Width -gt 0 -and $Width -lt $w) { $small = New-Object System.Drawing.Bitmap $bmp, $Width, ([int]($h * $Width / $w)); $bmp.Dispose(); $bmp = $small }
    $png = [IO.Path]::ChangeExtension($f.FullName, '.png')
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host $png
}
