# Reads the running RIP3.exe (already unpacked by PECompact) read-only,
# dumps the in-memory image and maps IAT-like pointers to DLL exports.
# Run with 32-bit PowerShell: the game is a 32-bit process.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out = Join-Path $root 'local\dump'
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Mem {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out int read);
  [DllImport("kernel32.dll")]
  public static extern bool CloseHandle(IntPtr h);
  public static byte[] Read(IntPtr h, long addr, int size) {
    var buf = new byte[size];
    for (int off = 0; off < size; off += 0x1000) {
      int n = Math.Min(0x1000, size - off);
      var page = new byte[n]; int got;
      if (ReadProcessMemory(h, new IntPtr(addr + off), page, n, out got)) Buffer.BlockCopy(page, 0, buf, off, got);
    }
    return buf;
  }
}
"@

# Attach to the game the owner launched from Steam (it refuses to start any other way)
$proc = Get-Process RIP3 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { throw 'RIP3.exe is not running. Launch it from Steam first.' }
$h = [Mem]::OpenProcess(0x0410, $false, $proc.Id)  # QUERY_INFORMATION | VM_READ
$mods = $proc.Modules
$main = $mods | Where-Object { $_.ModuleName -ieq 'RIP3.exe' }
$base = $main.BaseAddress.ToInt64()
$size = $main.ModuleMemorySize
$img = [Mem]::Read($h, $base, $size)

# Rewrite section headers so file layout == memory layout
$pe = [BitConverter]::ToInt32($img, 0x3C)
$n = [BitConverter]::ToInt16($img, $pe + 6)
$sec = $pe + 24 + [BitConverter]::ToInt16($img, $pe + 20)
for ($i = 0; $i -lt $n; $i++) {
  $o = $sec + 40 * $i
  $va = [BitConverter]::ToInt32($img, $o + 12)
  $vs = [BitConverter]::ToInt32($img, $o + 8)
  [BitConverter]::GetBytes($vs).CopyTo($img, $o + 16)
  [BitConverter]::GetBytes($va).CopyTo($img, $o + 20)
  [BitConverter]::GetBytes([int]0xE0000060).CopyTo($img, $o + 36)  # code|data|RWX
}
[IO.File]::WriteAllBytes((Join-Path $out 'RIP3_dump.exe'), $img)

# Export map of every loaded module (32-bit PE export directories)
$exp = @{}
foreach ($m in $mods) {
  if ($m.ModuleName -ieq 'RIP3.exe') { continue }
  $mb = $m.BaseAddress.ToInt64()
  $hdr = [Mem]::Read($h, $mb, 0x1000)
  $mpe = [BitConverter]::ToInt32($hdr, 0x3C)
  $edRva = [BitConverter]::ToInt32($hdr, $mpe + 0x78)
  $edSize = [BitConverter]::ToInt32($hdr, $mpe + 0x7C)
  if ($edRva -eq 0) { continue }
  $ed = [Mem]::Read($h, $mb + $edRva, 40)
  $cnt = [BitConverter]::ToInt32($ed, 24)
  $fn = [BitConverter]::ToInt32($ed, 28); $nm = [BitConverter]::ToInt32($ed, 32); $ord = [BitConverter]::ToInt32($ed, 36)
  if ($cnt -le 0 -or $cnt -gt 20000) { continue }
  $funcs = [Mem]::Read($h, $mb + $fn, [BitConverter]::ToInt32($ed, 20) * 4)
  $names = [Mem]::Read($h, $mb + $nm, $cnt * 4)
  $ords = [Mem]::Read($h, $mb + $ord, $cnt * 2)
  for ($k = 0; $k -lt $cnt; $k++) {
    $nameRva = [BitConverter]::ToInt32($names, $k * 4)
    $s = [Mem]::Read($h, $mb + $nameRva, 128)
    $name = [Text.Encoding]::ASCII.GetString($s).Split([char]0)[0]
    $fi = [BitConverter]::ToUInt16($ords, $k * 2)
    $frva = [BitConverter]::ToInt32($funcs, $fi * 4)
    if ($frva -ge $edRva -and $frva -lt $edRva + $edSize) { continue }  # forwarder
    $exp[[uint32]($mb + $frva)] = "$($m.ModuleName)!$name"
  }
}

# Find dwords in the dump that point at exports (IAT candidates)
$rows = New-Object System.Collections.Generic.List[string]
for ($o = 0; $o -le $img.Length - 4; $o += 4) {
  $v = [BitConverter]::ToUInt32($img, $o)
  if ($exp.ContainsKey($v)) { $rows.Add(('0x{0:X8},{1}' -f ($base + $o), $exp[$v])) }
}
$rows | Set-Content (Join-Path $out 'iat_map.csv') -Encoding utf8
$mods | ForEach-Object { '{0,-24} 0x{1:X8} {2}' -f $_.ModuleName, $_.BaseAddress.ToInt64(), $_.ModuleMemorySize } | Set-Content (Join-Path $out 'modules.txt') -Encoding utf8

[Mem]::CloseHandle($h) | Out-Null
"base=0x{0:X8} size={1} iat_hits={2} modules={3}" -f $base, $size, $rows.Count, $mods.Count
