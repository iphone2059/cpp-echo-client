param(
    [Parameter(Mandatory)]
    [string] $ClientPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Count actual bytes received on each connection; aggregate client metrics alone
# cannot detect a fast session consuming another session's finite quota.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
public sealed class CECQuotaPeer : IDisposable {
    private readonly TcpListener listener;
    private readonly Task<long[]> completion;
    public int Port { get; private set; }
    public CECQuotaPeer(int sessions, int slowMilliseconds, int expectedBytes) {
        listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        completion = Task.Run(async () => {
            var peers = new Task<long>[sessions];
            for (int i = 0; i < sessions; i++) {
                var client = await listener.AcceptTcpClientAsync();
                int delay = i == 1 ? slowMilliseconds : 0;
                int session = i;
                peers[i] = Task.Run(async () => {
                    using (client) {
                        client.NoDelay = true;
                        var stream = client.GetStream();
                        return await EchoConnectionAsync(stream, expectedBytes, delay, session);
                    }
                });
            }
            return await Task.WhenAll(peers);
        });
    }
    public static async Task<long> EchoConnectionAsync(Stream stream, int expectedBytes, int delay, int session) {
        var bytes = new byte[8192];
        long received = 0;
        long echoed = 0;
        for (;;) {
            int count;
            try {
                count = await stream.ReadAsync(bytes, 0, bytes.Length);
            } catch (IOException error) when (
                error.InnerException is SocketException socketError &&
                socketError.SocketErrorCode == SocketError.ConnectionReset &&
                received == expectedBytes && echoed == expectedBytes) {
                // The quota contract verifies data, not a graceful-close handshake.
                // Only this final read may accept reset, after every echo write completed.
                Console.WriteLine("quota_peer_terminal_reset session={0} received={1} echoed={2} native_error={3}",
                    session, received, echoed, socketError.ErrorCode);
                return received;
            }
            if (count == 0) {
                if (received != expectedBytes || echoed != expectedBytes)
                    throw new InvalidDataException("quota peer closed before its complete echo quota");
                return received;
            }
            received += count;
            if (received > expectedBytes)
                throw new InvalidDataException("quota peer received bytes beyond its per-session quota");
            if (delay != 0) await Task.Delay(delay);
            // Write errors, including reset with received==quota but echoed<quota, remain failures.
            await stream.WriteAsync(bytes, 0, count);
            echoed += count;
        }
    }
    public long[] Finish() {
        if (!completion.Wait(5000)) throw new TimeoutException("quota peer did not finish");
        return completion.GetAwaiter().GetResult();
    }
    public void Dispose() { listener.Stop(); }
}
// Deterministic write-only failure; the real peer loop must not classify it as terminal read closure.
public sealed class CECQuotaWriteFailureStream : Stream {
    public override bool CanRead { get { return true; } }
    public override bool CanWrite { get { return true; } }
    public override bool CanSeek { get { return false; } }
    public override long Length { get { throw new NotSupportedException(); } }
    public override long Position {
        get { throw new NotSupportedException(); }
        set { throw new NotSupportedException(); }
    }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
    public override void SetLength(long value) { throw new NotSupportedException(); }
    public override int Read(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
    public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellation) {
        buffer[offset] = 0;
        return Task.FromResult(1);
    }
    public override Task WriteAsync(byte[] buffer, int offset, int count, CancellationToken cancellation) {
        return Task.FromException(new IOException("injected echo write reset",
            new SocketException((int)SocketError.ConnectionReset)));
    }
}
'@

function Test-QuotaPeerTerminal {
    param([string]$Name, [int]$InitialBytes, [bool]$ReadEcho, [int]$ExtraBytes = 0,
          [bool]$Reset = $false, [string]$ExpectedFailure = '')
    $peer = [CECQuotaPeer]::new(1, 0, 1)
    $socket = [System.Net.Sockets.TcpClient]::new()
    try {
        $socket.Connect([System.Net.IPAddress]::Loopback, $peer.Port)
        $socket.ReceiveTimeout = 2000
        $socket.SendTimeout = 2000
        $stream = $socket.GetStream()
        if ($InitialBytes -gt 0) {
            $bytes = [byte[]]::new($InitialBytes)
            $stream.Write($bytes, 0, $bytes.Length)
            if ($ReadEcho) {
                $read = 0
                while ($read -lt $bytes.Length) {
                    $count = $stream.Read($bytes, $read, $bytes.Length - $read)
                    if ($count -eq 0) { throw 'quota terminal regression lost its initial echo' }
                    $read += $count
                }
                if (@($bytes | Where-Object { $_ -ne 0 }).Count -ne 0) {
                    throw 'quota terminal regression received corrupted data'
                }
            }
        }
        if ($ExtraBytes -gt 0) {
            $extra = [byte[]]::new($ExtraBytes)
            $stream.Write($extra, 0, $extra.Length)
        }
        if ($Reset) {
            $socket.Client.LingerState = [System.Net.Sockets.LingerOption]::new($true, 0)
            $socket.Client.Close()
        } else {
            $socket.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
        }
        $failure = $null
        $counts = $null
        try { $counts = $peer.Finish() } catch { $failure = $_.Exception.GetBaseException() }
        if ($ExpectedFailure -eq '') {
            if ($null -ne $failure) { throw $failure }
            if ($counts.Length -ne 1 -or $counts[0] -ne 1) { throw 'quota terminal regression lost its exact count' }
        } elseif ($ExpectedFailure -eq 'reset') {
            if ($failure -isnot [System.Net.Sockets.SocketException] -or
                $failure.SocketErrorCode -ne [System.Net.Sockets.SocketError]::ConnectionReset) {
                throw "quota terminal regression did not reject early reset: $failure"
            }
        } elseif ($failure -isnot [System.IO.InvalidDataException]) {
            throw "quota terminal regression did not reject missing or extra bytes: $failure"
        }
        Write-Host "PASS quota peer $Name"
    } finally {
        $socket.Dispose()
        $peer.Dispose()
    }
}

function Test-QuotaPeerWriteFailure {
    $stream = [CECQuotaWriteFailureStream]::new()
    try {
        $failure = $null
        try {
            $task = [CECQuotaPeer]::EchoConnectionAsync($stream, 1, 0, 0)
            $null = $task.GetAwaiter().GetResult()
        } catch { $failure = $_.Exception.GetBaseException() }
        if ($failure -isnot [System.Net.Sockets.SocketException] -or
            $failure.SocketErrorCode -ne [System.Net.Sockets.SocketError]::ConnectionReset) {
            throw "quota peer swallowed a failed echo write after receiving its quota: $failure"
        }
        Write-Host 'PASS quota peer rejects reset during echo write with received quota and incomplete echoed quota'
    } finally { $stream.Dispose() }
}

function Test-SessionQuota {
    param([int]$Sessions, [int]$Count, [int]$Depth, [int]$Workers,
          [int]$Cq = 4096, [int]$SlowMilliseconds = 0)
    $peer = [CECQuotaPeer]::new($Sessions, $SlowMilliseconds, $Count)
    $outputPath = Join-Path $env:TEMP ("cec_quota_" + [Guid]::NewGuid().ToString('N') + '.out')
    $errorPath = [System.IO.Path]::ChangeExtension($outputPath, '.err')
    $client = $null
    try {
        $client = Start-Process -FilePath $ClientPath -ArgumentList @('127.0.0.1', '/p', 'tcp',
            '/r', $peer.Port, '/c', $Sessions, '/threads', $Workers, '/n', $Count,
            '/k', $Depth, '/z', '1', '/cq', $Cq, '/q', '/stats') -PassThru -NoNewWindow `
            -RedirectStandardOutput $outputPath -RedirectStandardError $errorPath
        if (-not $client.WaitForExit(10000)) { throw 'quota client did not finish' }
        $output = Get-Content -LiteralPath $outputPath -Raw
        $errors = Get-Content -LiteralPath $errorPath -Raw
        if ($client.ExitCode -ne 0) { throw "quota client exited $($client.ExitCode): $errors" }
        $counts = $peer.Finish()
        foreach ($received in $counts) {
            if ($received -ne $Count) {
                throw "per-session quota /n $Count /k ${Depth}: received [$($counts -join ',')]"
            }
        }
        $total = $Sessions * $Count
        if ($output -notmatch "\bechoed=$total\b" -or $output -notmatch '\blost=0\b' -or
            $output -notmatch '\bcorrupted=0\b') { throw "incorrect quota metrics: $output" }
        Write-Host "PASS /c $Sessions /n $Count /k $Depth /threads ${Workers}: every connection received $Count echo(s)"
    } finally {
        if ($null -ne $client) {
            if (-not $client.HasExited) { $client.Kill($true) }
            $client.Dispose()
        }
        $peer.Dispose()
        Remove-Item -LiteralPath $outputPath, $errorPath -Force -ErrorAction SilentlyContinue
    }
}

Test-QuotaPeerTerminal -Name 'accepts exact quota followed by FIN' -InitialBytes 1 -ReadEcho $true
Test-QuotaPeerTerminal -Name 'accepts exact quota followed by reset' -InitialBytes 1 -ReadEcho $true -Reset $true
Test-QuotaPeerTerminal -Name 'rejects reset before quota' -InitialBytes 0 -ReadEcho $false -Reset $true -ExpectedFailure 'reset'
Test-QuotaPeerTerminal -Name 'rejects FIN before quota' -InitialBytes 0 -ReadEcho $false -ExpectedFailure 'missing'
Test-QuotaPeerTerminal -Name 'rejects initial excess bytes' -InitialBytes 2 -ReadEcho $false -ExpectedFailure 'extra'
Test-QuotaPeerTerminal -Name 'rejects an extra byte after its complete echo' -InitialBytes 1 -ReadEcho $true -ExtraBytes 1 -ExpectedFailure 'extra'
Test-QuotaPeerWriteFailure
Test-SessionQuota -Sessions 2 -Count 1 -Depth 8 -Workers 1
Test-SessionQuota -Sessions 2 -Count 5 -Depth 1 -Workers 1 -SlowMilliseconds 50
Test-SessionQuota -Sessions 3 -Count 5 -Depth 3 -Workers 2
Test-SessionQuota -Sessions 64 -Count 1 -Depth 1 -Workers 2 -Cq 64
