# Builds the mod DLLs and the injector into build\, and converts the
# translations (src\lang\*.txt, UTF-8) to CP1251 in build\lang\.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$zig = (Get-ChildItem "$root\tools\zig" -Recurse -Filter zig.exe | Select-Object -First 1).FullName
New-Item -ItemType Directory -Force "$root\build\lang" | Out-Null

& $zig cc -target x86-windows-gnu -O2 -shared -o "$root\build\rip3ru.dll" "$root\src\rip3ru.c"
if ($LASTEXITCODE) { throw "rip3ru.dll build failed" }
& $zig cc -target x86-windows-gnu -O2 -shared -o "$root\build\rip3video.dll" "$root\src\rip3video.c" -lgdi32 -luser32 -lwinmm
if ($LASTEXITCODE) { throw "rip3video.dll build failed" }
& $zig cc -target x86-windows-gnu -O2 -shared -o "$root\build\rip3net.dll" "$root\src\rip3net.c" -lws2_32 -luser32 -lgdi32
if ($LASTEXITCODE) { throw "rip3net.dll build failed" }
& $zig cc -target x86-windows-gnu -O2 -o "$root\build\rip3coop.exe" "$root\src\rip3coop.c" -lws2_32 -luser32
if ($LASTEXITCODE) { throw "rip3coop.exe build failed" }
& $zig cc -target x86-windows-gnu -O2 -o "$root\build\inject.exe" "$root\src\inject.c"
if ($LASTEXITCODE) { throw "inject.exe build failed" }

$cp = [Text.Encoding]::GetEncoding(1251)
foreach ($f in Get-ChildItem "$root\src\lang" -Filter *.txt) {
    # the game font (Images/Interface/font.png) has no Ё/ё glyphs
    $text = [IO.File]::ReadAllText($f.FullName, [Text.Encoding]::UTF8).Replace([char]0x451, [char]0x435).Replace([char]0x401, [char]0x415)
    [IO.File]::WriteAllBytes("$root\build\lang\$($f.Name)", $cp.GetBytes($text))
}
# pictures with baked-in text (needs game\data.pak, the workspace copy)
& "$PSScriptRoot\img\translate_images.ps1" | Out-Null
& "$PSScriptRoot\img\translate_images2.ps1" | Out-Null
"build ok"
