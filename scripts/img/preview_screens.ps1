# Assembles every tiled menu screen in data.pak (and a few single images) into
# previews under local\pak\screens\, to find baked-in English text.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
Add-Type -AssemblyName System.Drawing, System.IO.Compression, System.IO.Compression.FileSystem
$out = "$root\local\pak\screens"
New-Item -ItemType Directory -Force $out | Out-Null
$z = [IO.Compression.ZipFile]::OpenRead("$root\game\data.pak")

function Load($e) { $ms = New-Object IO.MemoryStream; $s = $e.Open(); $s.CopyTo($ms); $s.Close(); [Drawing.Image]::FromStream($ms) }

# group "<prefix>_NN.ext" tiles
$groups = $z.Entries | Where-Object { $_.FullName -match '^Images/Menu2?/(.+)_(\d\d)\.(jpg|png)$' } |
    Group-Object { $_.FullName -replace '_\d\d\.(jpg|png)$', '' }
foreach ($g in $groups) {
    $tiles = $g.Group | Sort-Object FullName
    $imgs = @($tiles | ForEach-Object { Load $_ })
    $tw = $imgs[0].Width; $th = $imgs[0].Height; $n = $imgs.Count
    # 12 tiles = 4x3 screen; otherwise lay them out in one row (2 rows if more than 4)
    $cols = if ($n -eq 12) { 4 } elseif ($n -eq 9) { 3 } elseif ($n -le 4) { $n } else { [Math]::Ceiling($n / 2) }
    $rows = [Math]::Ceiling($n / $cols)
    $bmp = New-Object Drawing.Bitmap ([int]($cols * $tw)), ([int]($rows * $th))
    $gr = [Drawing.Graphics]::FromImage($bmp)
    $gr.Clear([Drawing.Color]::FromArgb(40, 40, 70))
    for ($i = 0; $i -lt $n; $i++) { $gr.DrawImage($imgs[$i], [int](($i % $cols) * $tw), [int]([Math]::Floor($i / $cols) * $th), $imgs[$i].Width, $imgs[$i].Height) }
    $s = [Math]::Min(1.0, 900.0 / $bmp.Width)
    $small = New-Object Drawing.Bitmap $bmp, ([int]($bmp.Width * $s)), ([int]($bmp.Height * $s))
    $name = ($g.Name -split '/')[-1]
    $small.Save("$out\all_$name.png")
    "$name : $n tiles of ${tw}x$th"
    $gr.Dispose(); $bmp.Dispose(); $small.Dispose(); $imgs | ForEach-Object { $_.Dispose() }
}
$z.Dispose()
