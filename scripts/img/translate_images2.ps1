# Redraws English text baked into pictures on busy backgrounds: text pixels are
# found by colour inside given rectangles, the background is inpainted, and the
# Russian text is drawn with a style close to the original.
# Output goes to build\img\<path inside data.pak> (same byte size as the original:
# JPEG/PNG padded with zeros), previews to local\pak\screens\*_ru.png.
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
Add-Type -AssemblyName System.Drawing, System.IO.Compression, System.IO.Compression.FileSystem
if (-not ('ImgTools' -as [type])) { Add-Type -Path "$PSScriptRoot\ImgTools.cs" -ReferencedAssemblies System.Drawing, System.IO.Compression }
. "$PSScriptRoot\specs.ps1"   # defines $specs

$z = [IO.Compression.ZipFile]::OpenRead("$root\game\data.pak")
New-Item -ItemType Directory -Force "$root\local\pak\screens" | Out-Null
$cl = [Drawing.Color]::FromArgb(0, 0, 0, 0)

foreach ($s in $specs) {
    if ($Only -and $s.image -notlike "*$Only*") { continue }
    $tiled = $s.tiles -gt 0
    if ($tiled) {
        $ext = if ($z.GetEntry("$($s.image)_00.jpg")) { 'jpg' } else { 'png' }
        $orig = [ImgTools]::Assemble($z, $s.image, $ext, $s.tiles, $s.cols)
    } else { $orig = [ImgTools]::LoadEntry($z, $s.image); $ext = [IO.Path]::GetExtension($s.image).TrimStart('.') }
    $bmp = New-Object Drawing.Bitmap $orig

    $mask = New-Object 'bool[,]' $bmp.Width, $bmp.Height
    foreach ($e in $s.erase) {
        $rect = New-Object Drawing.Rectangle $e[0], $e[1], $e[2], $e[3]
        if ($e[4] -eq 'rows') { [ImgTools]::FillRows($bmp, $rect); continue }
        [ImgTools]::AddMask($mask, $orig, $rect, $e[4], $e[5])
    }
    [ImgTools]::Inpaint($bmp, $mask)

    foreach ($t in $s.text) {
        # x, y, w, h, align, maxW, text, font, bold, color, color2, outline, outlineW, sizeK
        $rect = New-Object Drawing.Rectangle $t[0], $t[1], $t[2], $t[3]
        $sizeK = if ($t.Count -gt 13) { [float]$t[13] } else { 1.3 }
        [ImgTools]::DrawStyled($bmp, $rect, $t[6], $t[7], [bool]$t[8], [ImgTools]::Hex($t[9]), [ImgTools]::Hex($t[10]),
                               [ImgTools]::Hex($t[11]), [float]$t[12], [int]$t[4], [float]$t[5], $sizeK)
    }
    foreach ($a in $s.arc) {
        # cx, cy, radius, text, font, bold, size px, colour, outline colour, outline px, letter spacing
        [ImgTools]::DrawArc($bmp, [float]$a[0], [float]$a[1], [float]$a[2], $a[3], $a[4], [bool]$a[5], [float]$a[6],
                            [ImgTools]::Hex($a[7]), [ImgTools]::Hex($a[8]), [float]$a[9], [float]$a[10])
    }
    $name = ($s.image -split '/')[-1] -replace '\.\w+$', ''
    $bmp.Save("$root\local\pak\screens\${name}_ru.png")

    if ($tiled) {
        $tw = [int]($orig.Width / $s.cols); $th = [int]($orig.Height / [Math]::Ceiling($s.tiles / $s.cols))
        for ($i = 0; $i -lt $s.tiles; $i++) {
            $entry = '{0}_{1:00}.{2}' -f $s.image, $i, $ext
            $x0 = ($i % $s.cols) * $tw; $y0 = [Math]::Floor($i / $s.cols) * $th
            $changed = $false
            for ($y = $y0; $y -lt $y0 + $th -and -not $changed; $y += 2) {
                for ($x = $x0; $x -lt $x0 + $tw; $x += 2) { if ($bmp.GetPixel($x, $y).ToArgb() -ne $orig.GetPixel($x, $y).ToArgb()) { $changed = $true; break } }
            }
            if (-not $changed) { continue }
            $max = [int]$z.GetEntry($entry).Length
            if ($ext -eq 'jpg') { $data = [ImgTools]::TileJpeg($bmp, $i, $s.cols, $tw, $th, $max) }
            else { $tile = $bmp.Clone((New-Object Drawing.Rectangle $x0, $y0, $tw, $th), $bmp.PixelFormat); $otile = $orig.Clone((New-Object Drawing.Rectangle $x0, $y0, $tw, $th), $orig.PixelFormat); $data = [ImgTools]::PngPadded($tile, $max, $otile); $tile.Dispose(); $otile.Dispose() }
            if (-not $data) { Write-Warning "$entry does not fit into $max bytes"; continue }
            $dst = Join-Path "$root\build\img" ($entry -replace '/', '\')
            New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
            [IO.File]::WriteAllBytes($dst, $data)
            "$entry"
        }
    } else {
        $max = [int]$z.GetEntry($s.image).Length
        $data = if ($ext -eq 'png') { [ImgTools]::PngPadded($bmp, $max, $orig) } else { [ImgTools]::TileJpeg($bmp, 0, 1, $bmp.Width, $bmp.Height, $max) }
        if (-not $data) { Write-Warning "$($s.image) does not fit into $max bytes" }
        else {
            $dst = Join-Path "$root\build\img" ($s.image -replace '/', '\')
            New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
            [IO.File]::WriteAllBytes($dst, $data)
            "$($s.image)"
        }
    }
    $orig.Dispose(); $bmp.Dispose()
}
$z.Dispose()
