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
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
public sealed class CECQuotaPeer : IDisposable {
    private readonly TcpListener listener;
    private readonly Task<long[]> completion;
    public int Port { get; private set; }
    public CECQuotaPeer(int sessions, int slowMilliseconds) {
        listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        completion = Task.Run(async () => {
            var peers = new Task<long>[sessions];
            for (int i = 0; i < sessions; i++) {
                var client = await listener.AcceptTcpClientAsync();
                int delay = i == 1 ? slowMilliseconds : 0;
                peers[i] = Task.Run(async () => {
                    using (client) {
                        client.NoDelay = true;
                        var stream = client.GetStream();
                        var bytes = new byte[8192];
                        long total = 0;
                        int count;
                        while ((count = await stream.ReadAsync(bytes, 0, bytes.Length)) != 0) {
                            total += count;
                            if (delay != 0) await Task.Delay(delay);
                            await stream.WriteAsync(bytes, 0, count);
                        }
                        return total;
                    }
                });
            }
            return await Task.WhenAll(peers);
        });
    }
    public long[] Finish() {
        if (!completion.Wait(5000)) throw new TimeoutException("quota peer did not finish");
        return completion.GetAwaiter().GetResult();
    }
    public void Dispose() { listener.Stop(); }
}
'@

function Test-SessionQuota {
    param([int]$Sessions, [int]$Count, [int]$Depth, [int]$Workers,
          [int]$Cq = 4096, [int]$SlowMilliseconds = 0)
    $peer = [CECQuotaPeer]::new($Sessions, $SlowMilliseconds)
    $outputPath = Join-Path $env:TEMP ("cec_quota_" + [Guid]::NewGuid().ToString('N') + '.out')
    $errorPath = [System.IO.Path]::ChangeExtension($outputPath, '.err')
    $client = $null
    try {
        $client = Start-Process -FilePath $ClientPath -ArgumentList @('127.0.0.1', '/p', 'tcp',
            '/r', $peer.Port, '/c', $Sessions, '/threads', $Workers, '/n', $Count,
            '/k', $Depth, '/z', '1', '/cq', $Cq, '/q', '/stats') -PassThru -WindowStyle Hidden `
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

Test-SessionQuota -Sessions 2 -Count 1 -Depth 8 -Workers 1
Test-SessionQuota -Sessions 2 -Count 5 -Depth 1 -Workers 1 -SlowMilliseconds 50
Test-SessionQuota -Sessions 3 -Count 5 -Depth 3 -Workers 2
Test-SessionQuota -Sessions 64 -Count 1 -Depth 1 -Workers 2 -Cq 64
