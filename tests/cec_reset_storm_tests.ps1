param(
    [Parameter(Mandatory)][string] $ClientPath,
    [string] $Label = 'echo client'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Invoke-CECProcess {
    param([string]$Path, [string[]]$Arguments, [int]$TimeoutMilliseconds = 10000)
    $psi = [Diagnostics.ProcessStartInfo]::new($Path)
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($arg in $Arguments) { $psi.ArgumentList.Add($arg) }
    $artifact = Join-Path (Split-Path -Parent $Path) ('reset-storm-artifacts/' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff') + '-' + [Guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $artifact -Force)
    $p = [Diagnostics.Process]::Start($psi)
    try {
        $stdout = $p.StandardOutput.ReadToEndAsync(); $stderr = $p.StandardError.ReadToEndAsync()
        $timedOut = -not $p.WaitForExit($TimeoutMilliseconds)
        if ($timedOut) {
            $p.Kill($true)
            if (-not $p.WaitForExit(5000)) { throw "process cleanup timeout: $Path" }
        }
        if (-not $stdout.Wait(5000) -or -not $stderr.Wait(5000)) { throw "process output timeout: $Path" }
        [IO.File]::WriteAllText((Join-Path $artifact 'stdout.log'), $stdout.Result)
        [IO.File]::WriteAllText((Join-Path $artifact 'stderr.log'), $stderr.Result)
        [IO.File]::WriteAllText((Join-Path $artifact 'run.json'), ([ordered]@{
            arguments=$Arguments; exit_code=$p.ExitCode; timed_out=$timedOut; executable=$Path
        } | ConvertTo-Json -Depth 4))
        if ($timedOut) { throw "process timeout: $Path $Arguments; artifacts=$artifact" }
        [pscustomobject]@{ Code=$p.ExitCode; Text=$stdout.Result; ErrorText=$stderr.Result }
    } finally {
        if (-not $p.HasExited) {
            $p.Kill($true)
            if (-not $p.WaitForExit(5000)) { throw "process final cleanup timeout: $Path" }
        }
        $p.Dispose()
    }
}
if (-not ('CECResetStormListener' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
public sealed class CECResetStormListener : IDisposable {
    readonly TcpListener listener = new TcpListener(IPAddress.Loopback, 0);
    readonly CancellationTokenSource stop = new CancellationTokenSource();
    readonly ConcurrentBag<Task> handlers = new ConcurrentBag<Task>();
    readonly ConcurrentBag<TcpClient> clients = new ConcurrentBag<TcpClient>();
    readonly Task loop;
    readonly int resetBudget;
    int accepts, resets, echoes;
    public int Port { get; private set; }
    public int Resets { get { return Volatile.Read(ref resets); } }
    public int Echoes { get { return Volatile.Read(ref echoes); } }
    public CECResetStormListener(int resetBudget) {
        this.resetBudget = resetBudget;
        listener.Start(); Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        loop = Run();
    }
    async Task Run() {
        try {
            while (!stop.IsCancellationRequested) {
                var client = await listener.AcceptTcpClientAsync(stop.Token);
                clients.Add(client);
                int ordinal = Interlocked.Increment(ref accepts);
                handlers.Add(Handle(client, ordinal));
            }
        } catch (OperationCanceledException) { }
        catch (SocketException) { if (!stop.IsCancellationRequested) throw; }
    }
    async Task Handle(TcpClient client, int ordinal) {
        try {
            using (client) {
                client.NoDelay = true;
                var stream = client.GetStream();
                var bytes = new byte[4096];
                while (true) {
                    int count = await stream.ReadAsync(bytes, stop.Token);
                    if (count == 0) return;
                    if (ordinal <= resetBudget) {
                        client.Client.LingerState = new LingerOption(true, 0);
                        Interlocked.Increment(ref resets);
                        return;
                    }
                    await stream.WriteAsync(bytes.AsMemory(0, count), stop.Token);
                    Interlocked.Increment(ref echoes);
                }
            }
        } catch (OperationCanceledException) { }
        catch (IOException) { }
        catch (SocketException) { }
        catch (ObjectDisposedException) { if (!stop.IsCancellationRequested) throw; }
    }
    public void Dispose() {
        stop.Cancel(); listener.Stop();
        foreach (var client in clients) client.Dispose();
        if (!loop.Wait(3000)) throw new Exception("reset listener accept timeout");
        if (!Task.WaitAll(handlers.ToArray(), 3000)) throw new Exception("reset listener handler timeout");
        stop.Dispose();
    }
}
'@
}
# The zero-reset case proves that the same drain/reconnect guards accept normal I/O.
foreach ($budget in @(0, 32)) {
    $listener = [CECResetStormListener]::new($budget)
    try {
        $result = Invoke-CECProcess $ClientPath @('127.0.0.1','/p','tcp','/r',[string]$listener.Port,
            '/n','0','/w','3','/rc','0','/t','1','/z','1024','/k','4','/c','8','/threads','4','/q','/stats') 15000
        $expected = if ($budget -eq 0) { 0 } else { 3 }
        if ($result.Code -ne $expected) { throw "reset budget=$budget exit=$($result.Code): $($result.ErrorText) $($result.Text)" }
        if ($listener.Resets -ne $budget -or $listener.Echoes -eq 0) { throw "reset budget=$budget resets=$($listener.Resets) echoes=$($listener.Echoes)" }
        $final = @($result.Text -split '\r?\n' | Where-Object { $_ -match '^final ' }) | Select-Object -Last 1
        if ($final -notmatch 'echoed=[1-9][0-9]*\b' -or $final -notmatch 'active=0\b' -or $final -notmatch 'pending=0\b') {
            throw "reset recovery did not echo and drain all generations: $final"
        }
        if ($result.ErrorText.Trim().Length -ne 0) { throw "reset recovery stderr: $($result.ErrorText)" }
        Write-Host "PASS $Label reset budget=$budget, echoed after resets and drained: $final"
    } finally { $listener.Dispose() }
}
