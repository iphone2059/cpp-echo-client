param(
    [Parameter(Mandatory)][string] $ServerPath,
    [Parameter(Mandatory)][string] $ClientPath,
    [ValidateSet('tcp','udp')][string] $Protocol = 'tcp',
    [int] $Sessions = 8,
    [int] $Payload = 1024,
    [int] $ClientWorkers = 4,
    [int] $ServerWorkers = 4,
    [int] $UdpDepth = 64,
    [int] $Seconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The TCP and UDP port namespaces are independent, so the probe socket must use the protocol the
# workload is about to bind.
function Get-FreePort {
    param([string] $ForProtocol)
    if ($ForProtocol -eq 'tcp') {
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
        $listener.Start()
        try { return $listener.LocalEndpoint.Port } finally { $listener.Stop() }
    }
    $probe = [System.Net.Sockets.UdpClient]::new(0)
    try { return $probe.Client.LocalEndPoint.Port } finally { $probe.Dispose() }
}

# Per-worker hard checks: deliveries never exceed arms, at most one notification stays in flight
# (gap == 1 is a legal shutdown state), the gap agrees with the counters, starvation wakeups are
# zero, the line declares the expected role and protocol, and the worker ids are exactly 0..n-1.
function Read-NotifyDiagnostics {
    param(
        [string] $Path,
        [Parameter(Mandatory)][string] $Role,
        [Parameter(Mandatory)][string] $ForProtocol,
        [Parameter(Mandatory)][int] $ExpectedWorkers
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Role diagnostics file missing: $Path" }
    $rows = @()
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -notmatch '^role=(\w+) protocol=(\w+) worker=(\d+) notify_arms=(\d+) notify_deliveries=(\d+) notify_gap=(\d+) timeout_wakeups_while_outstanding=(\d+)$') {
            throw "$Role diagnostics line is not parsable: $line"
        }
        $row = [pscustomobject]@{
            role = $Matches[1]; protocol = $Matches[2]; worker = [int]$Matches[3]; arms = [uint64]$Matches[4]
            deliveries = [uint64]$Matches[5]; gap = [uint64]$Matches[6]; timeouts = [uint64]$Matches[7]
        }
        if ($row.role -ne $Role) { throw "$Role diagnostics declared role '$($row.role)'" }
        if ($row.protocol -ne $ForProtocol) { throw "$Role diagnostics declared protocol '$($row.protocol)'" }
        if ($row.arms -lt $row.deliveries) { throw "$Role worker $($row.worker): deliveries exceed arms" }
        if (($row.arms - $row.deliveries) -gt 1) { throw "$Role worker $($row.worker): more than one notification in flight" }
        if ($row.gap -ne ($row.arms - $row.deliveries)) { throw "$Role worker $($row.worker): gap disagrees with the counters" }
        if ($row.timeouts -ne 0) { throw "$Role worker $($row.worker): $($row.timeouts) starvation wakeups" }
        $rows += $row
    }
    if ($rows.Count -ne $ExpectedWorkers) {
        throw "$Role diagnostics reported $($rows.Count) workers, expected $ExpectedWorkers"
    }
    $workerIds = @($rows | ForEach-Object { $_.worker } | Sort-Object)
    for ($index = 0; $index -lt $workerIds.Count; ++$index) {
        if ($workerIds[$index] -ne $index) { throw "$Role worker ids are not contiguous from zero: $($workerIds -join ',')" }
    }
    return $rows
}

$clientDiag = Join-Path $env:TEMP ('cec-' + [Guid]::NewGuid().ToString('N') + '.diag')
$serverDiag = Join-Path $env:TEMP ('ces-' + [Guid]::NewGuid().ToString('N') + '.diag')
$clientOut = Join-Path $env:TEMP ('cec-' + [Guid]::NewGuid().ToString('N') + '.out')
$clientErr = Join-Path $env:TEMP ('cec-' + [Guid]::NewGuid().ToString('N') + '.err')
$serverOut = Join-Path $env:TEMP ('ces-' + [Guid]::NewGuid().ToString('N') + '.out')
$serverErr = Join-Path $env:TEMP ('ces-' + [Guid]::NewGuid().ToString('N') + '.err')
$port = Get-FreePort -ForProtocol $Protocol
$env:CEC_DIAG_FILE = $clientDiag
$env:CES_DIAG_FILE = $serverDiag
$serverArgs = @('/p', $Protocol, '/s', [string]$port, '/w', [string]($Seconds + 10), '/q', '/stats')
if ($Protocol -eq 'tcp') { $serverArgs += @('/threads', [string]$ServerWorkers, '/cq', '4096', '/memory', '67108864') }
else { $serverArgs += @('/k', [string]$UdpDepth, '/cq', '8192', '/memory', '268435456') }
$server = $null
$clientExit = $null
$serverExit = $null
try {
    $server = Start-Process -FilePath $ServerPath -ArgumentList $serverArgs -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 700
    $clientArgs = @('127.0.0.1', '/p', $Protocol, '/r', [string]$port, '/n', '0', '/w', [string]$Seconds,
                    '/c', [string]$Sessions, '/threads', [string]$ClientWorkers, '/z', [string]$Payload, '/q', '/stats')
    if ($Protocol -eq 'tcp') { $clientArgs += @('/k', '8') }
    $client = Start-Process -FilePath $ClientPath -ArgumentList $clientArgs -RedirectStandardOutput $clientOut -RedirectStandardError $clientErr -PassThru -WindowStyle Hidden
    if (-not $client.WaitForExit(($Seconds + 30) * 1000)) { $client.Kill($true); throw 'client did not stop' }
    $clientExit = $client.ExitCode
    if (-not $server.WaitForExit(25000)) { $server.Kill($true); throw 'server did not stop on its own' }
    $serverExit = $server.ExitCode
} finally {
    if ($server -and -not $server.HasExited) { $server.Kill($true) | Out-Null; $server.WaitForExit(5000) | Out-Null }
    Remove-Item Env:CEC_DIAG_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:CES_DIAG_FILE -ErrorAction SilentlyContinue
}

# A gate must prove the workload itself succeeded, not only that the counters are consistent.
if ($clientExit -ne 0) { throw "client exited with code $clientExit" }
if ($serverExit -ne 0) { throw "server exited with code $serverExit" }
foreach ($pair in @(@('client', $clientErr), @('server', $serverErr))) {
    if ((Get-Item -LiteralPath $pair[1]).Length -ne 0) {
        throw "$($pair[0]) wrote stderr: " + ((Get-Content -LiteralPath $pair[1] -Raw).Trim())
    }
}

$clientRows = @(Read-NotifyDiagnostics -Path $clientDiag -Role 'client' -ForProtocol $Protocol -ExpectedWorkers $ClientWorkers)
$serverRows = @(Read-NotifyDiagnostics -Path $serverDiag -Role 'server' -ForProtocol $Protocol -ExpectedWorkers $(if ($Protocol -eq 'tcp') { $ServerWorkers } else { 1 }))
$clientText = [string](Get-Content -LiteralPath $clientOut -Raw)
$clientFinal = ($clientText -split "`r?`n" | Where-Object { $_ -match '^final' }) -join ''
$echoPerSec = 'n/a'
if ($clientFinal -match 'echo_per_sec=([0-9.]+)') { $echoPerSec = $Matches[1] }
Write-Output ('[PASS] {0} sessions={1} payload={2} client-workers={3} server-workers={4} udp-depth={5} client-exit={6} server-exit={7}' -f $Protocol, $Sessions, $Payload, $ClientWorkers, $ServerWorkers, $UdpDepth, $clientExit, $serverExit)
Write-Output ('       echo_per_sec={0}' -f $echoPerSec)
foreach ($row in ($clientRows + $serverRows)) {
    Write-Output ('       {0} {1} worker={2} arms={3} deliveries={4} gap={5} timeouts={6}' -f $row.role, $row.protocol, $row.worker, $row.arms, $row.deliveries, $row.gap, $row.timeouts)
}
Remove-Item -LiteralPath $clientDiag, $serverDiag, $clientOut, $clientErr, $serverOut, $serverErr -Force -ErrorAction SilentlyContinue
Write-Output 'NOTIFY DIAGNOSTICS GATE: PASS'