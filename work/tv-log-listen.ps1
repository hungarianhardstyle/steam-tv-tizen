# Listens for the TV diagnostic log lines and appends them to work/tv-log.txt.
# The TV app sends one UDP datagram per log line to 192.168.1.9:9999 (see probe.c).
$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot 'tv-log.txt'
$udp = New-Object System.Net.Sockets.UdpClient(9999)
("{0} --- listener started on udp/9999 ---" -f (Get-Date -Format 'HH:mm:ss.fff')) | Add-Content -Path $out -Encoding UTF8
$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
while ($true) {
    $data = $udp.Receive([ref]$remote)
    $line = [Text.Encoding]::UTF8.GetString($data)
    ("{0} {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $line) | Add-Content -Path $out -Encoding UTF8
}
