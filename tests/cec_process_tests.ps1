param(
    [Parameter(Mandatory)]
    [string] $ClientPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ClientPath -PathType Leaf)) {
    throw "client executable not found: $ClientPath"
}

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return ([System.Net.IPEndPoint] $listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Get-FreeUdpPort {
    $socket = [System.Net.Sockets.UdpClient]::new(0, [System.Net.Sockets.AddressFamily]::InterNetwork)
    try {
        return ([System.Net.IPEndPoint] $socket.Client.LocalEndPoint).Port
    } finally {
        $socket.Dispose()
    }
}

$tcpPort = Get-FreeTcpPort
$tcpJob = Start-Job -ArgumentList $tcpPort -ScriptBlock {
    param($Port)
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $listener.Start()
    try {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $buffer = [byte[]]::new(7919)
            while (($count = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $offset = 0
                while ($offset -lt $count) {
                    $chunk = [Math]::Min(997, $count - $offset)
                    $stream.Write($buffer, $offset, $chunk)
                    $offset += $chunk
                    Start-Sleep -Milliseconds 1
                }
            }
        } finally {
            $client.Dispose()
        }
    } finally {
        $listener.Stop()
    }
}
try {
    Start-Sleep -Milliseconds 500
    $tcpOutput = @(& $ClientPath 127.0.0.1 /p tcp /r $tcpPort /n 5 /k 4 /z 4096 /c 1 /threads 1 /q /stats)
    if ($LASTEXITCODE -ne 0) { throw "TCP client exited with code $LASTEXITCODE" }
    $tcpText = $tcpOutput -join "`n"
    if ($tcpText -notmatch 'echoed=5' -or $tcpText -notmatch 'corrupted=0' -or $tcpText -notmatch 'lost=0' -or
        $tcpText -notmatch 'network_errors=0' -or $tcpText -notmatch 'p50_us~' -or $tcpText -notmatch 'p99_us~' -or
        $tcpText -notmatch 'p999_us~' -or $tcpText -notmatch 'max_us~' -or
        $tcpText -notmatch 'latency_sample=batch') {
        throw "TCP client metrics do not expose the batch-latency contract: $tcpText"
    }
    if (-not (Wait-Job -Job $tcpJob -Timeout 5)) { throw 'TCP peer did not complete' }
    Receive-Job -Job $tcpJob -ErrorAction Stop | Out-Null
} finally {
    Stop-Job -Job $tcpJob -ErrorAction SilentlyContinue
    Remove-Job -Job $tcpJob -Force -ErrorAction SilentlyContinue
}

$heldTcpPort = Get-FreeTcpPort
$heldTcpJob = Start-Job -ArgumentList $heldTcpPort -ScriptBlock {
    param($Port)
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $listener.Start()
    try {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $payload = [byte[]]::new(4096)
            $offset = 0
            while ($offset -lt $payload.Length) {
                $count = $stream.Read($payload, $offset, $payload.Length - $offset)
                if ($count -eq 0) { return }
                $offset += $count
            }
            $stream.Write($payload, 0, $payload.Length)
            Start-Sleep -Seconds 3
        } finally {
            $client.Dispose()
        }
    } finally {
        $listener.Stop()
    }
}
try {
    Start-Sleep -Milliseconds 500
    $heldTcpClient = Start-Process -FilePath $ClientPath -ArgumentList @('127.0.0.1', '/p', 'tcp', '/r',
        $heldTcpPort, '/n', '0', '/z', '4096', '/t', '30', '/w', '1', '/c', '1', '/threads', '1', '/q') `
        -PassThru -WindowStyle Hidden
    try {
        $heldTcpClient.WaitForExit(5000) | Out-Null
        if (-not $heldTcpClient.HasExited -or $heldTcpClient.ExitCode -ne 0) {
            throw 'TCP client did not drain a held RIO operation cleanly'
        }
    } finally {
        if (-not $heldTcpClient.HasExited) { $heldTcpClient.Kill($true) }
        $heldTcpClient.Dispose()
    }
} finally {
    Stop-Job -Job $heldTcpJob -ErrorAction SilentlyContinue
    Remove-Job -Job $heldTcpJob -Force -ErrorAction SilentlyContinue
}

$noEchoPort = Get-FreeTcpPort
$noEchoReadyName = "Local\cec_no_echo_$([Guid]::NewGuid().ToString('N'))"
$noEchoReady = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset,
    $noEchoReadyName)
$noEchoJob = Start-Job -ArgumentList $noEchoPort, $noEchoReadyName -ScriptBlock {
    param($Port, $ReadyName)
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $ready = [System.Threading.EventWaitHandle]::OpenExisting($ReadyName)
    $listener.Start()
    try {
        [void] $ready.Set()
        $client = $listener.AcceptTcpClient()
        try {
            Start-Sleep -Seconds 3
        } finally {
            $client.Dispose()
        }
    } finally {
        $ready.Dispose()
        $listener.Stop()
    }
}
try {
    if (-not $noEchoReady.WaitOne(5000)) { throw 'no-echo TCP peer did not become ready' }
    & $ClientPath 127.0.0.1 /p tcp /r $noEchoPort /n 0 /z 4096 /t 30 /w 1 /c 1 /threads 1 /q
    if ($LASTEXITCODE -ne 0) { throw "controlled stop before first echo exited with code $LASTEXITCODE" }
} finally {
    Stop-Job -Job $noEchoJob -ErrorAction SilentlyContinue
    Remove-Job -Job $noEchoJob -Force -ErrorAction SilentlyContinue
    $noEchoReady.Dispose()
}

$eofPort = Get-FreeTcpPort
$eofReadyName = "Local\cec_eof_$([Guid]::NewGuid().ToString('N'))"
$eofReady = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset,
    $eofReadyName)
$eofJob = Start-Job -ArgumentList $eofPort, $eofReadyName -ScriptBlock {
    param($Port, $ReadyName)
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $ready = [System.Threading.EventWaitHandle]::OpenExisting($ReadyName)
    $listener.Start()
    try {
        [void] $ready.Set()
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $payload = [byte[]]::new(4096)
            $offset = 0
            while ($offset -lt $payload.Length) {
                $count = $stream.Read($payload, $offset, $payload.Length - $offset)
                if ($count -eq 0) { return }
                $offset += $count
            }
            $stream.Write($payload, 0, 2048)
        } finally {
            $client.Dispose()
        }
    } finally {
        $ready.Dispose()
        $listener.Stop()
    }
}
try {
    if (-not $eofReady.WaitOne(5000)) { throw 'EOF TCP peer did not become ready' }
    $eofOutput = @(& $ClientPath 127.0.0.1 /p tcp /r $eofPort /n 1 /z 4096 /t 5 /c 1 /threads 1 /stats)
    if ($LASTEXITCODE -ne 3) { throw "short TCP EOF exited with code $LASTEXITCODE instead of 3" }
    $eofText = $eofOutput -join "`n"
    if ($eofText -notmatch 'corrupted=0' -or $eofText -notmatch 'lost=1') {
        throw "short TCP EOF was not classified as connection loss: $eofText"
    }
} finally {
    Stop-Job -Job $eofJob -ErrorAction SilentlyContinue
    Remove-Job -Job $eofJob -Force -ErrorAction SilentlyContinue
    $eofReady.Dispose()
}

$udpPort = Get-FreeUdpPort
$udpReadyName = "Local\cec_udp_$([Guid]::NewGuid().ToString('N'))"
$udpReady = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset,
    $udpReadyName)
$udpJob = Start-Job -ArgumentList $udpPort, $udpReadyName -ScriptBlock {
    param($Port, $ReadyName)
    $socket = [System.Net.Sockets.UdpClient]::new($Port, [System.Net.Sockets.AddressFamily]::InterNetwork)
    $ready = [System.Threading.EventWaitHandle]::OpenExisting($ReadyName)
    try {
        [void] $ready.Set()
        for ($index = 0; $index -lt 5; $index++) {
            $source = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $payload = $socket.Receive([ref] $source)
            [void] $socket.Send($payload, $payload.Length, $source)
        }
    } finally {
        $ready.Dispose()
        $socket.Dispose()
    }
}
try {
    if (-not $udpReady.WaitOne(5000)) { throw 'UDP peer did not become ready' }
    $udpOutput = @(& $ClientPath 127.0.0.1 /p udp /r $udpPort /n 5 /z 65507 /c 1 /threads 1 /stats)
    if ($LASTEXITCODE -ne 0) { throw "UDP client exited with code $LASTEXITCODE" }
    $udpText = $udpOutput -join "`n"
    if ($udpText -notmatch 'echoed=5' -or $udpText -notmatch 'corrupted=0' -or $udpText -notmatch 'lost=0' -or
        $udpText -notmatch 'network_errors=0' -or $udpText -notmatch 'latency_sample=batch') {
        throw "UDP client finite acceptance metrics are incomplete: $udpText"
    }
    if (-not (Wait-Job -Job $udpJob -Timeout 5)) { throw 'UDP peer did not complete' }
    Receive-Job -Job $udpJob -ErrorAction Stop | Out-Null
} finally {
    Stop-Job -Job $udpJob -ErrorAction SilentlyContinue
    Remove-Job -Job $udpJob -Force -ErrorAction SilentlyContinue
    $udpReady.Dispose()
}

$heldUdpPort = Get-FreeUdpPort
$heldUdpReadyName = "Local\cec_held_udp_$([Guid]::NewGuid().ToString('N'))"
$heldUdpReady = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset,
    $heldUdpReadyName)
$heldUdpJob = Start-Job -ArgumentList $heldUdpPort, $heldUdpReadyName -ScriptBlock {
    param($Port, $ReadyName)
    $socket = [System.Net.Sockets.UdpClient]::new($Port, [System.Net.Sockets.AddressFamily]::InterNetwork)
    $ready = [System.Threading.EventWaitHandle]::OpenExisting($ReadyName)
    try {
        [void] $ready.Set()
        $source = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
        $first = $socket.Receive([ref] $source)
        [void] $socket.Send($first, $first.Length, $source)
        [void] $socket.Receive([ref] $source)
        Start-Sleep -Seconds 3
    } finally {
        $ready.Dispose()
        $socket.Dispose()
    }
}
try {
    if (-not $heldUdpReady.WaitOne(5000)) { throw 'held UDP peer did not become ready' }
    $heldUdpClient = Start-Process -FilePath $ClientPath -ArgumentList @('127.0.0.1', '/p', 'udp', '/r',
        $heldUdpPort, '/n', '0', '/z', '1200', '/t', '30', '/w', '1', '/c', '1', '/threads', '1', '/q') `
        -PassThru -WindowStyle Hidden
    try {
        $heldUdpClient.WaitForExit(5000) | Out-Null
        if (-not $heldUdpClient.HasExited -or $heldUdpClient.ExitCode -ne 0) {
            throw 'UDP client did not drain a held RIO operation cleanly'
        }
    } finally {
        if (-not $heldUdpClient.HasExited) { $heldUdpClient.Kill($true) }
        $heldUdpClient.Dispose()
    }
} finally {
    Stop-Job -Job $heldUdpJob -ErrorAction SilentlyContinue
    Remove-Job -Job $heldUdpJob -Force -ErrorAction SilentlyContinue
    $heldUdpReady.Dispose()
}

Write-Host 'PASS client TCP/UDP fragmented echo and forced-stop drain scenarios'
