# Finds text boxes of a given colour kind on a data.pak image and writes a
# numbered preview (local\pak\detect\<name>_<kind>.png) plus the box list.
# Usage: detect.ps1 -Image Images/Menu2/m_controls -Tiles 12 -Cols 4 -Kind green
#        detect.ps1 -Image Images/Interface/interface_03.png -Kind green
param([string]$Image, [int]$Tiles = 0, [int]$Cols = 4, [string]$Kind = 'green', [int]$Gap = 8, [int]$Index = -1)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
Add-Type -AssemblyName System.Drawing, System.IO.Compression, System.IO.Compression.FileSystem
if (-not ('ImgTools' -as [type])) { Add-Type -Path "$PSScriptRoot\ImgTools.cs" -ReferencedAssemblies System.Drawing, System.IO.Compression }
$z = [IO.Compression.ZipFile]::OpenRead("$root\game\data.pak")
if ($Tiles -gt 0) {
    $ext = if ($z.GetEntry("${Image}_00.jpg")) { 'jpg' } else { 'png' }
    $bmp = [ImgTools]::Assemble($z, $Image, $ext, $Tiles, $Cols)
} else { $bmp = [ImgTools]::LoadEntry($z, $Image) }
$z.Dispose()
if ($Index -ge 0) {   # one 1024x1024 page out of a horizontal strip
    $page = $bmp.Clone((New-Object Drawing.Rectangle ($Index * 1024), 0, 1024, $bmp.Height), $bmp.PixelFormat); $bmp.Dispose(); $bmp = $page
}
$boxes = [ImgTools]::FindBoxes($bmp, (New-Object Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height), $Gap, $Kind)
$vis = New-Object Drawing.Bitmap $bmp.Width, $bmp.Height
$g = [Drawing.Graphics]::FromImage($vis)
$g.Clear([Drawing.Color]::FromArgb(60, 60, 90)); $g.DrawImage($bmp, 0, 0, $bmp.Width, $bmp.Height)
$pen = New-Object Drawing.Pen ([Drawing.Color]::Red), 1
$font = New-Object Drawing.Font 'Arial', 9, ([Drawing.FontStyle]::Bold)
for ($i = 0; $i -lt $boxes.Count; $i++) {
    $b = $boxes[$i]
    $g.DrawRectangle($pen, $b)
    $g.DrawString("$i", $font, [Drawing.Brushes]::Yellow, $b.Right + 1, $b.Top - 2)
    "{0,3}: {1},{2} {3}x{4}" -f $i, $b.X, $b.Y, $b.Width, $b.Height
}
$name = ($Image -split '/')[-1] -replace '\.\w+$', ''
if ($Index -ge 0) { $name += "_$Index" }
New-Item -ItemType Directory -Force "$root\local\pak\detect" | Out-Null
$vis.Save("$root\local\pak\detect\${name}_$Kind.png")
$g.Dispose(); $vis.Dispose(); $bmp.Dispose()
