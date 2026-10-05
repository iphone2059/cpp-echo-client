param(
    [Parameter(Mandatory)][string] $ServerPath,
    [Parameter(Mandatory)][string] $ClientPath,
    [ValidateSet('tcp','udp')][string] $Protocol = 'tcp',
    [int] $Sessions = 8,
    [int] $Payload = 1024,
    [int] $ServerWorkers = 4,
    [int] $UdpDepth = 64,
    [int] $Seconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FreePort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

# Per-worker hard checks: deliveries never exceed arms, at most one notification stays in flight
# (gap == 1 is a legal shutdown state), and starvation wakeups must be zero.
function Read-NotifyDiagnostics {
    param([string] $Path, [string] $Role)
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
        if ($row.arms -lt $row.deliveries) { throw "$Role worker $($row.worker): deliveries exceed arms" }
        if (($row.arms - $row.deliveries) -gt 1) { throw "$Role worker $($row.worker): more than one notification in flight" }
        if ($row.gap -ne ($row.arms - $row.deliveries)) { throw "$Role worker $($row.worker): gap disagrees with the counters" }
        if ($row.timeouts -ne 0) { throw "$Role worker $($row.worker): $($row.timeouts) starvation wakeups" }
        $rows += $row
    }
    if ($rows.Count -eq 0) { throw "$Role diagnostics file is empty: $Path" }
    return $rows
}

$clientDiag = Join-Path $env:TEMP ('cec-' + [Guid]::NewGuid().ToString('N') + '.diag')
$serverDiag = Join-Path $env:TEMP ('ces-' + [Guid]::NewGuid().ToString('N') + '.diag')
$clientOut = Join-Path $env:TEMP ('cec-' + [Guid]::NewGuid().ToString('N') + '.out')
$serverOut = Join-Path $env:TEMP ('ces-' + [Guid]::NewGuid().ToString('N') + '.out')
$port = Get-FreePort
$env:CEC_DIAG_FILE = $clientDiag
$env:CES_DIAG_FILE = $serverDiag
$serverArgs = @('/p', $Protocol, '/s', [string]$port, '/w', [string]($Seconds + 10), '/q', '/stats')
if ($Protocol -eq 'tcp') { $serverArgs += @('/threads', [string]$ServerWorkers, '/cq', '4096', '/memory', '67108864') }
else { $serverArgs += @('/k', [string]$UdpDepth, '/cq', '8192', '/memory', '268435456') }
$server = $null
try {
    $server = Start-Process -FilePath $ServerPath -ArgumentList $serverArgs -RedirectStandardOutput $serverOut -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 700
    $clientArgs = @('127.0.0.1', '/p', $Protocol, '/r', [string]$port, '/n', '0', '/w', [string]$Seconds,
                    '/c', [string]$Sessions, '/threads', '1', '/z', [string]$Payload, '/q', '/stats')
    if ($Protocol -eq 'tcp') { $clientArgs += @('/k', '8') }
    $client = Start-Process -FilePath $ClientPath -ArgumentList $clientArgs -RedirectStandardOutput $clientOut -PassThru -WindowStyle Hidden
    if (-not $client.WaitForExit(($Seconds + 30) * 1000)) { $client.Kill($true); throw 'client did not stop' }
    $client.WaitForExit()
} finally {
    if ($server -and -not $server.HasExited) { if (-not $server.WaitForExit(20000)) { $server.Kill($true) } }
    Remove-Item Env:CEC_DIAG_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:CES_DIAG_FILE -ErrorAction SilentlyContinue
}

$clientRows = @(Read-NotifyDiagnostics -Path $clientDiag -Role 'client')
$serverRows = @(Read-NotifyDiagnostics -Path $serverDiag -Role 'server')
$clientText = [string](Get-Content -LiteralPath $clientOut -Raw)
$clientFinal = ($clientText -split "`r?`n" | Where-Object { $_ -match '^final' }) -join ''
$echoPerSec = 'n/a'
if ($clientFinal -match 'echo_per_sec=([0-9.]+)') { $echoPerSec = $Matches[1] }
Write-Output ('[PASS] {0} sessions={1} payload={2} server-workers={3} client-workers={4} udp-depth={5}' -f $Protocol, $Sessions, $Payload, $ServerWorkers, $clientRows.Count, $UdpDepth)
Write-Output ('       echo_per_sec={0}' -f $echoPerSec)
foreach ($row in ($clientRows + $serverRows)) {
    Write-Output ('       {0} {1} worker={2} arms={3} deliveries={4} gap={5} timeouts={6}' -f $row.role, $row.protocol, $row.worker, $row.arms, $row.deliveries, $row.gap, $row.timeouts)
}
Remove-Item -LiteralPath $clientDiag, $serverDiag, $clientOut, $serverOut -Force -ErrorAction SilentlyContinue
Write-Output 'NOTIFY DIAGNOSTICS GATE: PASS'