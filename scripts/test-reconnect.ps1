# Tests the launcher's NAT-rebinding path without a second PC.
# A UDP socket (A) plays the friend at 127.0.0.1:27605. The service is forced to 27606
# because A owns 27605. A sends HELLO, then B sends HELLO from another port with the
# same nonce. The launcher must report "reconnect" and update its peer address.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root 'build\rip3coop.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'build\rip3coop.exe not found; run scripts\build.ps1' }

function Encode-Code([uint32]$ip, [uint16]$port) {
    $check = (([uint64]($ip -bxor (([uint64]$port * 40503) -band 0xFFFFFFFF))) * 2654435761) -band 0xFFFFFFFF
    $check = ($check -shr 20) -band 0xFFF
    $v = [uint64]$ip -bor ([uint64]$check -shl 33)
    $alpha = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
    $s = ''
    for ($i = 8; $i -ge 0; $i--) {
        $s += $alpha[[int](($v -shr ($i * 5)) -band 31)]
        if ($i -eq 4) { $s += '-' }
    }
    return $s
}

$service = $null
$ctrl = $null
$a = $null
$b = $null
try {
    Get-Process rip3coop -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500

    $a = New-Object Net.Sockets.UdpClient 27605
    $a.Client.ReceiveTimeout = 2000
    $b = New-Object Net.Sockets.UdpClient
    $ctrl = New-Object Net.Sockets.UdpClient 27601
    $ctrl.Client.ReceiveTimeout = 100

    $env:RIP3COOP_LOCAL_IP = '26.1.2.3'
    $service = Start-Process $exe -ArgumentList '--service', '1234' -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 700

    $code = Encode-Code 0x7F000001 27605
    function Send-Cmd([byte]$op, [string]$text) {
        $pkt = New-Object byte[] 66
        $pkt[0] = 0x82
        $pkt[1] = $op
        if ($text) { [Text.Encoding]::ASCII.GetBytes($text).CopyTo($pkt, 2) }
        [void]$ctrl.Send($pkt, 66, '127.0.0.1', 27600)
    }
    Send-Cmd 1 'radmin'
    Start-Sleep -Milliseconds 300
    Send-Cmd 3 $code

    $ep = New-Object Net.IPEndPoint([Net.IPAddress]::Loopback, 0)
    try { [void]$a.Receive([ref]$ep) } catch { throw 'service did not send HELLO' }

    function Send-Hello($sock, [uint16]$nonce) {
        $pkt = New-Object byte[] 11
        $pkt[0] = 1
        [BitConverter]::GetBytes([uint32]0x50433352).CopyTo($pkt, 1)
        $pkt[5] = 1
        $pkt[6] = 2
        [BitConverter]::GetBytes($nonce).CopyTo($pkt, 7)
        [BitConverter]::GetBytes([uint16]0).CopyTo($pkt, 9)
        [void]$sock.Send($pkt, 11, '127.0.0.1', 27606)
    }
    Send-Hello $a 0x1234
    Start-Sleep -Milliseconds 500
    Send-Hello $b 0x1234

    $connected = $false
    $reconnected = $false
    $until = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $until) {
        try { $d = $ctrl.Receive([ref]$ep) } catch { continue }
        if ($d.Length -lt 205 -or $d[0] -ne 0x83) { continue }
        $note = [Text.Encoding]::UTF8.GetString($d, 45, 160).Trim([char]0)
        if ($note -match 'Соединено') { $connected = $true }
        if ($note -match 'восстановлена') { $reconnected = $true; break }
    }

    if (-not $connected) { throw 'no CONNECTED state received' }
    if (-not $reconnected) { throw 'no reconnect message received' }
    'PASS: connect + NAT rebinding'
}
finally {
    if ($a) { $a.Close() }
    if ($b) { $b.Close() }
    if ($ctrl) { $ctrl.Close() }
    if ($service -and -not $service.HasExited) { $service.Kill() }
    Remove-Item Env:RIP3COOP_LOCAL_IP -ErrorAction SilentlyContinue
}
