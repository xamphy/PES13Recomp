# Compares two same-size PNGs: tools\diff-png.ps1 <a.png> <b.png> <out.png>
# Writes |a-b| amplified x4 and prints the share of pixels that differ by more than 8 levels.
param([string]$A, [string]$B, [string]$Out)
Add-Type -AssemblyName System.Drawing
$ia = [System.Drawing.Bitmap]::FromFile((Resolve-Path $A))
$ib = [System.Drawing.Bitmap]::FromFile((Resolve-Path $B))
$w = [Math]::Min($ia.Width, $ib.Width); $h = [Math]::Min($ia.Height, $ib.Height)
$o = New-Object System.Drawing.Bitmap $w, $h
$changed = 0
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $pa = $ia.GetPixel($x, $y); $pb = $ib.GetPixel($x, $y)
        $dr = [Math]::Abs($pa.R - $pb.R); $dg = [Math]::Abs($pa.G - $pb.G); $db = [Math]::Abs($pa.B - $pb.B)
        if ([Math]::Max($dr, [Math]::Max($dg, $db)) -gt 8) { $changed++ }
        $o.SetPixel($x, $y, [System.Drawing.Color]::FromArgb([Math]::Min(255, $dr * 4), [Math]::Min(255, $dg * 4), [Math]::Min(255, $db * 4)))
    }
}
$o.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"{0:P1} of pixels differ by more than 8 levels" -f ($changed / ($w * $h))
$ia.Dispose(); $ib.Dispose(); $o.Dispose()
