# Redraws English text baked into menu screens (data.pak tiles) in Russian.
# Output: build\img\<path inside data.pak> (only changed tiles, JPEG padded with
# zeros to the original size, because the game allocates the original size),
# plus full-screen previews in local\pak\screens\*_ru.png.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
Add-Type -AssemblyName System.Drawing, System.IO.Compression, System.IO.Compression.FileSystem
Add-Type -Path "$PSScriptRoot\ImgTools.cs" -ReferencedAssemblies System.Drawing, System.IO.Compression
$font = 'Bahnschrift Condensed'

function Rect($x, $y, $w, $h) { New-Object Drawing.Rectangle $x, $y, $w, $h }

# erase: rectangles whose green pixels are removed
# text:  x, y, w, h, align (0 left, 1 center), max width, Russian text
$screens = @(
    @{ prefix = 'Images/Menu2/m_options'; count = 12; cols = 4
       erase = @((Rect 440 198 145 36), (Rect 360 250 115 28), (Rect 360 284 115 34), (Rect 330 372 178 36), (Rect 340 406 160 64),
                 (Rect 586 250 110 28), (Rect 586 284 100 34), (Rect 570 338 95 34), (Rect 586 372 40 36), (Rect 586 406 64 64),
                 (Rect 354 504 64 32), (Rect 478 504 78 32), (Rect 614 504 66 32))
       text = @(
           @(420, 200, 184, 32, 1, 160, 'НАСТРОЙКИ'),
           @(326, 253, 184, 21, 1, 178, 'Громкость музыки'),
           @(326, 290, 184, 21, 1, 178, 'Громкость звука'),
           @(326, 380, 184, 21, 1, 180, 'Прозрачность интерфейса'),
           @(326, 430, 184, 21, 1, 180, 'Чувствительность мыши'),
           @(591, 253, 115, 21, 0, 112, 'Режим цветов'),
           @(591, 288, 115, 21, 0, 112, 'Режим крови'),
           @(530, 340, 175, 24, 1, 160, 'Сложность'),
           @(591, 378, 110, 24, 0, 108, 'Легко'),
           @(591, 412, 110, 24, 0, 108, 'Средне'),
           @(591, 446, 110, 24, 0, 108, 'Сложно'),
           @(330, 509, 110, 20, 1, 100, 'Игрок 1'),
           @(462, 509, 110, 22, 1, 110, 'Управление'),
           @(592, 509, 110, 20, 1, 100, 'Игрок 2')) },
    @{ prefix = 'Images/Menu2/m_stat'; count = 12; cols = 4
       erase = @((Rect 395 96 180 42), (Rect 150 160 200 40), (Rect 150 220 200 44), (Rect 150 276 200 44),
                 (Rect 150 388 200 44), (Rect 150 444 200 40), (Rect 150 500 200 44), (Rect 150 560 200 44))
       text = @(
           @(330, 102, 310, 28, 1, 300, 'Уровень пройден'),
           @(155, 167, 190, 24, 0, 188, 'Время уровня'),
           @(155, 228, 190, 24, 0, 188, 'Убито врагов'),
           @(155, 284, 190, 24, 0, 188, 'Точность'),
           @(155, 397, 190, 24, 0, 188, 'Опыт за уровень'),
           @(155, 452, 190, 24, 0, 188, 'Бонус за время'),
           @(155, 508, 190, 24, 0, 188, 'Бонус за точность'),
           @(155, 568, 190, 24, 0, 188, 'Всего опыта')) },
    @{ prefix = 'Images/Menu2/m_stat_hotseat'; count = 12; cols = 4
       erase = @((Rect 182 100 180 40), (Rect 228 162 196 40), (Rect 228 222 196 46), (Rect 228 278 196 46),
                 (Rect 228 390 196 44), (Rect 228 444 196 40), (Rect 228 501 196 44), (Rect 228 560 196 46))
       text = @(
           @(188, 108, 300, 26, 0, 290, 'Уровень пройден'),
           @(234, 170, 186, 24, 0, 184, 'Время уровня'),
           @(234, 232, 186, 24, 0, 184, 'Убито врагов'),
           @(234, 288, 186, 24, 0, 184, 'Точность'),
           @(234, 400, 186, 24, 0, 184, 'Опыт за уровень'),
           @(234, 454, 186, 24, 0, 184, 'Бонус за время'),
           @(234, 511, 186, 24, 0, 184, 'Бонус за точность'),
           @(234, 570, 186, 24, 0, 184, 'Всего опыта')) }
)

$z = [IO.Compression.ZipFile]::OpenRead("$root\game\data.pak")
New-Item -ItemType Directory -Force "$root\local\pak\screens" | Out-Null
foreach ($s in $screens) {
    $orig = [ImgTools]::Assemble($z, $s.prefix, 'jpg', $s.count, $s.cols)
    $bmp = $orig.Clone()
    $color = [ImgTools]::TextColor($orig, (Rect 0 0 $orig.Width $orig.Height))
    foreach ($e in $s.erase) { [ImgTools]::Erase($bmp, $e, 2, [Drawing.Color]::Black) }
    foreach ($t in $s.text) {
        [ImgTools]::DrawText($bmp, (Rect $t[0] $t[1] $t[2] $t[3]), $t[6], $font, $color, $t[4], $t[5])
    }
    $name = ($s.prefix -split '/')[-1]
    $bmp.Save("$root\local\pak\screens\${name}_ru.png")

    # write only tiles that differ from the original
    $tw = 256; $th = 256
    for ($i = 0; $i -lt $s.count; $i++) {
        $entry = '{0}_{1:00}.jpg' -f $s.prefix, $i
        $x0 = ($i % $s.cols) * $tw; $y0 = [Math]::Floor($i / $s.cols) * $th
        $changed = $false
        for ($y = $y0; $y -lt $y0 + $th -and -not $changed; $y += 2) {
            for ($x = $x0; $x -lt $x0 + $tw; $x += 2) {
                if ($bmp.GetPixel($x, $y).ToArgb() -ne $orig.GetPixel($x, $y).ToArgb()) { $changed = $true; break }
            }
        }
        if (-not $changed) { continue }
        $max = $z.GetEntry($entry).Length
        $data = [ImgTools]::TileJpeg($bmp, $i, $s.cols, $tw, $th, [int]$max)
        if (-not $data) { Write-Warning "$entry does not fit into $max bytes"; continue }
        $dst = Join-Path "$root\build\img" ($entry -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        [IO.File]::WriteAllBytes($dst, $data)
        "$entry ($max bytes)"
    }
    $orig.Dispose(); $bmp.Dispose()
}
$z.Dispose()
