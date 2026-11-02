# Builds dist\rip3-coop.zip from the current build\ output.
# Only release files are included: no logs, PDBs, caches, saves or game files.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
$out = Join-Path $dist 'rip3-coop.zip'

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE) { throw 'build failed' }
}

$required = @(
    'start_rip3.cmd', 'start_rip3.ps1', 'Запустить игру с модом.cmd',
    'build\rip3coop.exe', 'build\inject.exe',
    'build\rip3net.dll', 'build\rip3video.dll', 'build\rip3ru.dll',
    'build\rip3video.ini',
    'build\lang\levels.ru.txt', 'build\lang\perks.ru.txt', 'build\lang\weapons.ru.txt',
    'dist\README_RU.txt'
)
foreach ($rel in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $rel))) { throw "missing release file: $rel" }
}

# stage into a clean directory first, so the zip cannot pick up stale files
$stage = Join-Path $build 'package-stage'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

$map = [ordered]@{}
foreach ($rel in $required) {
    $name = if ($rel -eq 'dist\README_RU.txt') { 'README_RU.txt' } else { $rel.Replace('\', '/') }
    $map[$name] = Join-Path $root $rel
}
$map['README.md'] = Join-Path $root 'README.md'

# all translated menu pictures
$img = Join-Path $build 'img'
if (Test-Path -LiteralPath $img) {
    Get-ChildItem -LiteralPath $img -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($img.Length + 1).Replace('\', '/')
        $map["build/img/$rel"] = $_.FullName
    }
}

# copy the whitelist to the stage and hash it
$hashes = @()
foreach ($name in $map.Keys) {
    $src = $map[$name]
    $dst = Join-Path $stage $name.Replace('/', '\')
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $hashes += ('{0}  {1}' -f (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLower(), $name)
}
$commit = (git -C $root rev-parse --short HEAD 2>$null)
if (-not $commit) { $commit = 'unknown' }
if (git -C $root status --porcelain 2>$null) { $commit += '-dirty' }
@(
    "rip3-coop release",
    "commit: $commit",
    "built:  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
) | Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Encoding UTF8
# Windows PowerShell 5.1 reads a BOM-less file as ANSI; the manifest contains the
# Russian launcher name, so keep the UTF-8 BOM for Explorer/Notepad/PowerShell.
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'), $hashes, (New-Object Text.UTF8Encoding($true)))

# deterministic zip with forward slashes (Compress-Archive writes backslashes)
New-Item -ItemType Directory -Force $dist | Out-Null
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open($out, [IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        $entry = $zip.CreateEntry("rip3-coop/$rel", [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $_.LastWriteTime
        $in = [IO.File]::OpenRead($_.FullName)
        try { $es = $entry.Open(); try { $in.CopyTo($es) } finally { $es.Dispose() } }
        finally { $in.Dispose() }
    }
} finally { $zip.Dispose() }

$size = [math]::Round((Get-Item -LiteralPath $out).Length / 1MB, 2)
"packaged $out ($size MB, $($map.Count) files + VERSION/SHA256SUMS)"
