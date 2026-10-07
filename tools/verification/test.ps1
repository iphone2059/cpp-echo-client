param(
    [string] $Filter = '',
    [int] $SoakSeconds = 10,
    [int] $PerfSeconds = 5,
    [int] $PortBase = 26000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Identity is derived from the repository name, so this file is byte-identical in every
# implementation: the project is <implementation>-echo-<component>.
$Root           = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Project        = Split-Path $Root -Leaf
$ProjectPattern = '^(cpp|rust|zig|swift)-echo-(client|server)\z'
if ($Project -notmatch $ProjectPattern) {
    throw "unexpected project name: $Project"
}
$Implementation = $Matches[1]
$Component      = $Matches[2]
$PeerKind       = if ($Component -eq 'client') { 'server' } else { 'client' }
$ResultsDir = Join-Path $PSScriptRoot 'results'
$ArchiveDir = Join-Path $ResultsDir 'archive'
New-Item -ItemType Directory -Force -Path $ArchiveDir | Out-Null

$ExecutableFile = Join-Path $PSScriptRoot 'executable.txt'
$PeerFile       = Join-Path $PSScriptRoot ("peer-" + $PeerKind + ".txt")
$FeaturesFile   = Join-Path $PSScriptRoot 'features.txt'

if (-not (Test-Path -LiteralPath $ExecutableFile -PathType Leaf)) { throw "executable.txt is missing: $ExecutableFile" }
if (-not (Test-Path -LiteralPath $PeerFile -PathType Leaf)) { throw "$PeerKind file is missing: $PeerFile" }
$Executable = (Join-Path $PSScriptRoot ((Get-Content -LiteralPath $ExecutableFile -Raw).Trim()))
$Peer       = (Join-Path $PSScriptRoot ((Get-Content -LiteralPath $PeerFile -Raw).Trim()))
$Features   = @()
if (Test-Path -LiteralPath $FeaturesFile -PathType Leaf) {
    $Features = @(Get-Content -LiteralPath $FeaturesFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
}
$HasFeature = { param([string] $Name) return $Features -contains $Name }

foreach ($pair in @(@('executable', $Executable), @($PeerKind, $Peer))) {
    if (-not (Test-Path -LiteralPath $pair[1] -PathType Leaf)) {
        throw "$($pair[0]) not found: $($pair[1]) - edit the text file next to this script"
    }
}

$GitHead  = (git -C $Root rev-parse HEAD 2>$null)
$GitDirty = 'unknown'
if ($GitHead) { $GitDirty = (@(git -C $Root status --porcelain 2>$null).Count -gt 0).ToString().ToLower() } else { $GitHead = 'unknown' }

# The binary contract this kit verifies; the reference implementation defines the value.
$ContractFile    = Join-Path $PSScriptRoot 'contract-version.txt'
$ContractVersion = (Get-Content -LiteralPath $ContractFile -Raw).Trim()
if ($ContractVersion -ne 'echo-binary-contract-v1') {
    throw "contract-version.txt does not declare echo-binary-contract-v1"
}

$Columns = @('timestamp','project','implementation','component','git_head','git_dirty','executable','build_type',
    'test_id','category','protocol','result','exit_code','expected_exit_code','duration_ms','sessions','workers',
    'payload_bytes','pipeline_depth','echoed','attempted','pending','corrupted','lost','cancelled','connections',
    'reconnects','network_errors','echo_per_sec','mib_per_sec','p50_us','p99_us','p999_us','notify_arms',
    'notify_deliveries','notify_gap','notify_timeouts','stdout_valid','stderr_empty','message')
$Rows = [System.Collections.Generic.List[object]]::new()
$Script:Port = $PortBase

function Get-NextPort { $Script:Port = $Script:Port + 1; return $Script:Port }

function Save-Results {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $latest = Join-Path $ResultsDir 'latest.csv'
    $Rows | Export-Csv -LiteralPath $latest -NoTypeInformation -Encoding utf8
    Copy-Item -LiteralPath $latest -Destination (Join-Path $ArchiveDir ("test-" + $stamp + ".csv")) -Force
    $summary = $Rows | Group-Object category | ForEach-Object {
        [pscustomobject]@{
            category = $_.Name
            total    = $_.Count
            passed   = @($_.Group | Where-Object { $_.result -eq 'PASS' }).Count
            failed   = @($_.Group | Where-Object { $_.result -eq 'FAIL' }).Count
            skipped  = @($_.Group | Where-Object { $_.result -eq 'SKIP' }).Count
        }
    }
    $summary | Export-Csv -LiteralPath (Join-Path $ResultsDir 'latest-summary.csv') -NoTypeInformation -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $ResultsDir 'latest-summary.csv') -Destination (Join-Path $ArchiveDir ("summary-" + $stamp + ".csv")) -Force
}

function Add-Row {
    param([hashtable] $Values)
    $row = [ordered]@{}
    foreach ($column in $Columns) { $row[$column] = '' }
    $row['timestamp']        = (Get-Date).ToString('s')
    $row['project']          = $Project
    $row['implementation']   = $Implementation
    $row['component']        = $Component
    $row['git_head']         = $GitHead
    $row['git_dirty']        = $GitDirty
    $row['executable']       = $Executable
    $row['build_type']       = 'release'
    foreach ($key in $Values.Keys) { if ($row.Contains($key)) { $row[$key] = $Values[$key] } }
    $Rows.Add([pscustomobject]$row)
    Write-Host ("{0,-8} {1,-16} {2}" -f $row['result'], $row['test_id'], $row['message'])
}

function Convert-FinalLine {
    param([string] $Text)
    $fields = @{}
    $final = @($Text -split "\r?\n" | Where-Object { $_ -match '^final ' } | Select-Object -Last 1)
    if ($final.Count -eq 0) { return $fields }
    foreach ($match in [regex]::Matches($final[0], '([A-Za-z_0-9]+)[=~]([0-9]+(?:\.[0-9]+)?)')) {
        $fields[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $fields
}

function Convert-Diagnostics {
    param([string] $Path)
    $totals = @{ arms = ''; deliveries = ''; gap = ''; timeouts = '' }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $totals }
    $arms = 0; $deliveries = 0; $gap = 0; $timeouts = 0; $seen = $false
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -notmatch 'notify_arms=(\d+) notify_deliveries=(\d+) notify_gap=(\d+) timeout_wakeups_while_outstanding=(\d+)') { continue }
        $seen = $true
        $arms = $arms + [int64]$Matches[1]; $deliveries = $deliveries + [int64]$Matches[2]
        $gap = $gap + [int64]$Matches[3]; $timeouts = $timeouts + [int64]$Matches[4]
        if ([int64]$Matches[2] -gt [int64]$Matches[1]) { throw 'diagnostics: deliveries exceed arms' }
        if (([int64]$Matches[1] - [int64]$Matches[2]) -gt 1) { throw 'diagnostics: more than one notification in flight' }
        if ([int64]$Matches[3] -ne ([int64]$Matches[1] - [int64]$Matches[2])) { throw 'diagnostics: gap disagrees with the counters' }
        if ([int64]$Matches[4] -ne 0) { throw 'diagnostics: starvation wakeups detected' }
    }
    if (-not $seen) { return $totals }
    Write-Host ('  diagnostics ' + $Path + ' -> arms=' + $arms + ' deliveries=' + $deliveries)
    return @{ arms = $arms; deliveries = $deliveries; gap = $gap; timeouts = $timeouts }
}

function Get-FreePort {
    $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $probe.Start()
    $value = $probe.LocalEndpoint.Port
    $probe.Stop()
    return $value
}

function Test-Feature {
    param([string] $Name)
    return $Features -contains $Name
}

function Get-FreePort {
    $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $probe.Start()
    $value = $probe.LocalEndpoint.Port
    $probe.Stop()
    return $value
}

function Test-Feature {
    param([string] $Name)
    return $Features -contains $Name
}

function Read-TextFile {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    $content = Get-Content -LiteralPath $Path -Raw
    if ($null -eq $content) { return '' }
    return [string]$content
}

function Wait-ForServer {
    param([int] $Port, [string] $ForProtocol, [int] $TimeoutMs = 15000)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ($ForProtocol -ne 'tcp') { Start-Sleep -Milliseconds 600; return $true }
        $client = $null
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $client.Connect('127.0.0.1', $Port)
            return $true
        } catch {
            Start-Sleep -Milliseconds 200
        } finally {
            if ($client) { $client.Dispose() }
        }
    }
    return $false
}

function Invoke-Test {
    param(
        [string] $TestId,
        [string] $Category,
        [string] $Protocol,
        [string[]] $Arguments,
        [int] $ExpectedExit = 0,
        [string] $ExpectPattern = '',
        [string] $ExpectStream = 'any',
        [int[]] $AllowExitCodes = @(),
        [int] $TimeoutMs = 40000,
        [bool] $WithPeer = $true
    )
    if ($Filter -ne '' -and $TestId -notlike $Filter) { return }
    $port = Get-FreePort
    $tag = [Guid]::NewGuid().ToString('N')
    $outFile = Join-Path $env:TEMP ("verif-" + $tag + ".out")
    $errFile = Join-Path $env:TEMP ("verif-" + $tag + ".err")
    $diagFile = Join-Path $env:TEMP ("verif-" + $tag + ".diag")
    $peerOut = Join-Path $env:TEMP ("verif-peer-" + $tag + ".out")
    $peerErr = Join-Path $env:TEMP ("verif-peer-" + $tag + ".err")
    $arguments = @($Arguments | ForEach-Object { $_ -replace '@PORT@', [string]$port })
    if ($arguments -notcontains '/w') { $arguments += @('/w', '20') }
    $started = Get-Date
    $peerProcess = $null
    $process = $null
    $finished = $false
    $exit = -1
    $stdout = ''
    $stderr = ''
    $metrics = @{}
    $notify = @{ arms = ''; deliveries = ''; gap = ''; timeouts = '' }
    $failure = ''
    try {
        if ($WithPeer) {
            $peerArgs = @('127.0.0.1', '/p', $Protocol, '/r', [string]$port, '/n', '20', '/z', '256', '/q')
            if ($Component -eq 'client') { $peerArgs = @('/p', $Protocol, '/s', [string]$port, '/q') }
            $peerProcess = Start-Process -FilePath $Peer -ArgumentList $peerArgs -RedirectStandardOutput $peerOut -RedirectStandardError $peerErr -PassThru -WindowStyle Hidden
            Wait-ForServer -Port $port -ForProtocol $Protocol | Out-Null
        }
        if ($Component -eq 'client' -and (Test-Feature 'notify-diagnostics')) { $env:CEC_DIAG_FILE = $diagFile }
        if ($Component -eq 'server' -and (Test-Feature 'notify-diagnostics')) { $env:CES_DIAG_FILE = $diagFile }
        $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
        $process = Start-Process -FilePath $Executable -ArgumentList $quoted -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -WindowStyle Hidden
        $finished = $process.WaitForExit($TimeoutMs)
        if ($finished) { $exit = $process.ExitCode } else { $process.Kill($true) | Out-Null }
        $stdout = Read-TextFile -Path $outFile
        $stderr = Read-TextFile -Path $errFile
        $metrics = Convert-FinalLine -Text $stdout
        if ($peerProcess) { if (-not $peerProcess.WaitForExit(25000)) { $peerProcess.Kill($true) | Out-Null } }
        $notify = Convert-Diagnostics -Path $diagFile
    } catch {
        $failure = $_.Exception.Message
    } finally {
        Remove-Item Env:CEC_DIAG_FILE -ErrorAction SilentlyContinue
        Remove-Item Env:CES_DIAG_FILE -ErrorAction SilentlyContinue
        if ($peerProcess -and -not $peerProcess.HasExited) { $peerProcess.Kill($true) | Out-Null }
        $duration = [int]((Get-Date) - $started).TotalMilliseconds
        $result = 'FAIL'
        $message = ''
        if ($failure -ne '') { $message = 'case error: ' + $failure }
        elseif (-not $finished) { $message = 'timeout after ' + $TimeoutMs + ' ms' }
        elseif ($exit -ne $ExpectedExit -and ($AllowExitCodes -notcontains $exit)) { $message = 'exit ' + $exit + ', expected ' + $ExpectedExit }
        elseif ($exit -ne $ExpectedExit -and ([string]$metrics['corrupted']) -ne '0') { $message = 'tolerated exit ' + $exit + ' but the run reported corrupted echoes' }
        elseif ($ExpectPattern -ne '' -and (($stdout + [char]10 + $stderr) -notmatch $ExpectPattern)) { $message = 'output does not match ' + $ExpectPattern }
        elseif ($ExpectStream -eq 'stderr' -and $stderr.Trim() -eq '') { $message = 'expected the diagnostic on stderr, got none' }
        elseif ($ExpectStream -eq 'stderr' -and $stdout.Trim() -ne '') { $message = 'expected the diagnostic on stderr, got it on stdout' }
        elseif ($ExpectStream -eq 'stdout' -and $stdout.Trim() -eq '') { $message = 'expected the message on stdout, got none' }
        elseif ($ExpectedExit -eq 0 -and $stderr.Trim() -ne '') { $message = 'stderr: ' + $stderr.Trim() }
        else { $result = 'PASS'; $message = 'ok' }
        $expectedText = ''
        if ($finished) { $expectedText = [string]$ExpectedExit }
        Add-Row @{
            test_id = $TestId; category = $Category; protocol = $Protocol; result = $result
            exit_code = $exit; expected_exit_code = $expectedText; duration_ms = $duration
            sessions = $metrics['sessions']; workers = $metrics['workers']; payload_bytes = $metrics['bytes']
            pipeline_depth = $metrics['pipeline_depth']; echoed = $metrics['echoed']; attempted = $metrics['attempted']
            pending = $metrics['pending']; corrupted = $metrics['corrupted']; lost = $metrics['lost']
            cancelled = $metrics['cancelled']; connections = $metrics['connections']; reconnects = $metrics['reconnects']
            network_errors = $metrics['network_errors']; echo_per_sec = $metrics['echo_per_sec']
            mib_per_sec = $metrics['MiB_per_sec']; p50_us = $metrics['p50_us~']; p99_us = $metrics['p99_us~']
            p999_us = $metrics['p999_us~']; notify_arms = $notify.arms; notify_deliveries = $notify.deliveries
            notify_gap = $notify.gap; notify_timeouts = $notify.timeouts
            stdout_valid = ($stdout -match 'final ').ToString().ToLower()
            stderr_empty = ($stderr.Trim() -eq '').ToString().ToLower()
            message = $message
        }
        Remove-Item -LiteralPath $outFile, $errFile, $diagFile, $peerOut, $peerErr -Force -ErrorAction SilentlyContinue
    }
}


function Invoke-StopTest {
    param([string] $TestId, [string] $Category, [string] $Protocol, [string[]] $PeerArguments, [int] $TimeoutMs = 40000)
    if ($Filter -ne '' -and $TestId -notlike $Filter) { return }
    $port = Get-FreePort
    $tag = [Guid]::NewGuid().ToString('N')
    $outFile = Join-Path $env:TEMP ("verif-" + $tag + ".out")
    $errFile = Join-Path $env:TEMP ("verif-" + $tag + ".err")
    $diagFile = Join-Path $env:TEMP ("verif-" + $tag + ".diag")
    $peerOut = Join-Path $env:TEMP ("verif-peer-" + $tag + ".out")
    $peerErr = Join-Path $env:TEMP ("verif-peer-" + $tag + ".err")
    $started = Get-Date
    $server = $null
    $client = $null
    $serverDone = $false
    $serverExit = -1
    $clientDone = $false
    $serverErr = ''
    $metrics = @{}
    $notify = @{ arms = ''; deliveries = ''; gap = ''; timeouts = '' }
    $failure = ''
    try {
        if (Test-Feature 'notify-diagnostics') { $env:CES_DIAG_FILE = $diagFile }
        $server = Start-Process -FilePath $Executable -ArgumentList @('/p', $Protocol, '/s', [string]$port, '/w', '5', '/q', '/stats') -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -WindowStyle Hidden
        Wait-ForServer -Port $port -ForProtocol $Protocol | Out-Null
        $arguments = @($PeerArguments | ForEach-Object { $_ -replace '@PORT@', [string]$port })
        $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
        $client = Start-Process -FilePath $Peer -ArgumentList $quoted -RedirectStandardOutput $peerOut -RedirectStandardError $peerErr -PassThru -WindowStyle Hidden
        $clientDone = $client.WaitForExit($TimeoutMs)
        if (-not $clientDone) { $client.Kill($true) | Out-Null }
        $serverDone = $server.WaitForExit(25000)
        if ($serverDone) { $serverExit = $server.ExitCode } else { $server.Kill($true) | Out-Null }
        $serverErr = Read-TextFile -Path $errFile
        $peerMetrics = Convert-FinalLine -Text (Read-TextFile -Path $peerOut)
        $metrics = Convert-FinalLine -Text (Read-TextFile -Path $outFile)
        $notify = Convert-Diagnostics -Path $diagFile
    } catch {
        $failure = $_.Exception.Message
    } finally {
        Remove-Item Env:CES_DIAG_FILE -ErrorAction SilentlyContinue
        if ($server -and -not $server.HasExited) { $server.Kill($true) | Out-Null }
        if ($client -and -not $client.HasExited) { $client.Kill($true) | Out-Null }
        $duration = [int]((Get-Date) - $started).TotalMilliseconds
        $result = 'FAIL'
        $message = ''
        if ($failure -ne '') { $message = 'case error: ' + $failure }
        elseif (-not $serverDone) { $message = 'server did not stop' }
        elseif ($serverExit -ne 0) { $message = 'server exit ' + $serverExit }
        elseif ($serverErr.Trim() -ne '') { $message = 'server stderr: ' + $serverErr.Trim() }
        elseif (-not $clientDone) { $message = 'peer did not stop' }
        elseif ($Category -ne 'CLI' -and ([int64]($peerMetrics['echoed']) -le 0)) { $message = 'peer reported no echo traffic' }
        else { $result = 'PASS'; $message = 'ok' }
        Add-Row @{
            test_id = $TestId; category = $Category; protocol = $Protocol; result = $result
            exit_code = $serverExit; expected_exit_code = '0'; duration_ms = $duration
            sessions = $peerMetrics['sessions']; workers = $metrics['workers']; echoed = $peerMetrics['echoed']
            attempted = $metrics['attempted']; pending = $metrics['pending']; corrupted = $metrics['corrupted']
            lost = $metrics['lost']; cancelled = $metrics['cancelled']; connections = $metrics['connections']
            reconnects = $metrics['reconnects']; network_errors = $metrics['network_errors']
            echo_per_sec = $metrics['echo_per_sec']; mib_per_sec = $metrics['MiB_per_sec']
            p50_us = $metrics['p50_us']; p99_us = $metrics['p99_us']; p999_us = $metrics['p999_us']
            notify_arms = $notify.arms; notify_deliveries = $notify.deliveries
            notify_gap = $notify.gap; notify_timeouts = $notify.timeouts
            stdout_valid = ((Read-TextFile -Path $outFile) -match 'final ').ToString().ToLower()
            stderr_empty = ($serverErr.Trim() -eq '').ToString().ToLower()
            message = $message
        }
        Remove-Item -LiteralPath $outFile, $errFile, $diagFile, $peerOut, $peerErr -Force -ErrorAction SilentlyContinue
    }
}

$PortBase = $PortBase
$Soak = [string]$SoakSeconds
$Perf = [string]$PerfSeconds

$CliOne = if ($Component -eq 'client') { '/p' } else { '/p' }
$ErrorActionPreference = 'Continue'
if ($Component -eq 'client') {
    Invoke-Test -TestId 'CLI-001' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h') -ExpectedExit 0 -ExpectPattern 'Usage:' -WithPeer $false
    Invoke-Test -TestId 'CLI-002' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/p', 'sctp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments' -WithPeer $false
    Invoke-Test -TestId 'CLI-003' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/b', '1.0') -ExpectedExit 1 -WithPeer $false
    Invoke-Test -TestId 'CLI-004' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/unknown') -ExpectedExit 1 -WithPeer $false
    Invoke-Test -TestId 'PARSE-001' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/h', '/p', 'udp', '/k', '4') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'PARSE-002' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/h', '/p', 'tcp', '/s', '1234') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unknown-switch' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'PARSE-003' -Category 'PARSE' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/k', '0') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: out-of-range' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'PARSE-004' -Category 'PARSE' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/k', '65537') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: out-of-range' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'PARSE-005' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/zzz') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unknown-switch' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'ORDER-001' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/k', '4', '/p', 'udp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'ORDER-002' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/p', 'udp', '/k', '4') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'ORDER-003' -Category 'ORDER' -Protocol 'tcp' -Arguments @('127.0.0.1', '/k', '4', '/p', 'udp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'ORDER-004' -Category 'ORDER' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'udp', '/k', '4') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    # Missing arguments are part of the contract: the reference reports the missing target first and
    # the missing protocol second, each with one exact token.
    Invoke-Test -TestId 'MISSING-001' -Category 'MISSING' -Protocol 'tcp' -Arguments @() -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-target' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'MISSING-002' -Category 'MISSING' -Protocol 'tcp' -Arguments @('127.0.0.1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-protocol' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'MISSING-003' -Category 'MISSING' -Protocol 'tcp' -Arguments @('/p', 'tcp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-target' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'HELP-001' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/n', '1', '/d', 'x') -ExpectedExit 0 -ExpectPattern 'Usage:' -ExpectStream 'stdout' -WithPeer $false
    Invoke-Test -TestId 'HELP-002' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/b', '1.0') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: invalid-number' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'HELP-003' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/p', 'tcp', '/rc', '1', '/l', '127.0.0.1:1234') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: invalid-number' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'TCP-001' -Category 'TCP' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '20', '/z', '256', '/q', '/stats') -ExpectPattern 'echoed=20'
    Invoke-Test -TestId 'TCP-002' -Category 'TCP' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '100', '/k', '8', '/z', '1024', '/q', '/stats') -ExpectPattern 'echoed=100'
    Invoke-Test -TestId 'TCP-003' -Category 'TCP' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '10', '/d', 'echo from toolkit', '/q', '/stats') -ExpectPattern 'echoed=10'
    Invoke-Test -TestId 'UDP-001' -Category 'UDP' -Protocol 'udp' -Arguments @('127.0.0.1', '/p', 'udp', '/r', '@PORT@', '/n', '20', '/z', '1200', '/q', '/stats') -ExpectPattern 'echoed=20'
    # A one-byte datagram is the smallest legal payload. On a loaded host a single datagram can be lost,
    # and the reference classifies that as lost plus an echo failure, so exit 3 is tolerated here while
    # corruption never is.
    Invoke-Test -TestId 'UDP-002' -Category 'UDP' -Protocol 'udp' -Arguments @('127.0.0.1', '/p', 'udp', '/r', '@PORT@', '/n', '20', '/z', '1', '/q', '/stats') -ExpectPattern 'corrupted=0' -AllowExitCodes @(3)
    Invoke-Test -TestId 'QUOTA-001' -Category 'QUOTA' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '1', '/z', '256', '/q', '/stats') -ExpectPattern 'echoed=1'
    Invoke-Test -TestId 'QUOTA-002' -Category 'QUOTA' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', '2', '/z', '256', '/q', '/stats')
    Invoke-Test -TestId 'TIMEOUT-001' -Category 'TIMEOUT' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/t', '1', '/w', '5', '/q', '/stats') -ExpectedExit 3 -WithPeer $false
    Invoke-Test -TestId 'RECONNECT-001' -Category 'RECONNECT' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '5', '/rc', '1', '/z', '256', '/q', '/stats') -ExpectPattern 'echoed=5'
    Invoke-Test -TestId 'SHUTDOWN-001' -Category 'SHUTDOWN' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', '3', '/z', '256', '/q', '/stats')
    Invoke-Test -TestId 'SHUTDOWN-002' -Category 'SHUTDOWN' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', '1', '/c', '8', '/k', '4', '/z', '256', '/q', '/stats')
    Invoke-Test -TestId 'NOTIFY-001' -Category 'NOTIFY' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', '3', '/c', '4', '/z', '512', '/q', '/stats')
    Invoke-Test -TestId 'SOAK-001' -Category 'SOAK' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', $Soak, '/z', '1024', '/q', '/stats') -TimeoutMs (($SoakSeconds + 40) * 1000)
    Invoke-Test -TestId 'PERF-TCP-001' -Category 'PERF' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', $Perf, '/c', '8', '/k', '8', '/z', '4096', '/q', '/stats') -TimeoutMs (($PerfSeconds + 40) * 1000)
    Invoke-Test -TestId 'PERF-UDP-001' -Category 'PERF' -Protocol 'udp' -Arguments @('127.0.0.1', '/p', 'udp', '/r', '@PORT@', '/n', '0', '/w', $Perf, '/c', '8', '/z', '1200', '/q', '/stats') -TimeoutMs (($PerfSeconds + 40) * 1000)
} else {
    Invoke-Test -TestId 'CLI-001' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h') -ExpectedExit 0 -ExpectPattern 'Usage:' -WithPeer $false
    Invoke-Test -TestId 'CLI-002' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/p', 'sctp') -ExpectedExit 1 -WithPeer $false
    Invoke-Test -TestId 'CLI-003' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/b', '1.0') -ExpectedExit 1 -WithPeer $false
    Invoke-Test -TestId 'PARSE-001' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/h', '/p', 'udp', '/t', '1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'PARSE-002' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/zzz') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unknown-switch' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'ORDER-001' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/t', '1', '/p', 'udp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'ORDER-002' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/k', '4', '/p', 'tcp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr' -WithPeer $false
    # Missing arguments are part of the contract: the reference reports the missing protocol first and
    # rejects a stray target second.
    Invoke-Test -TestId 'MISSING-001' -Category 'MISSING' -Protocol 'tcp' -Arguments @() -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-protocol' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'MISSING-002' -Category 'MISSING' -Protocol 'tcp' -Arguments @('/s', '7000') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-protocol' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'MISSING-003' -Category 'MISSING' -Protocol 'tcp' -Arguments @('127.0.0.1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unexpected-target' -ExpectStream 'stderr' -WithPeer $false
    Invoke-Test -TestId 'HELP-001' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/p', 'tcp', '/s', '1234') -ExpectedExit 0 -ExpectPattern 'Usage:' -ExpectStream 'stdout' -WithPeer $false
    Invoke-StopTest -TestId 'TCP-001' -Category 'TCP' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '20', '/z', '256', '/q', '/stats')
    Invoke-StopTest -TestId 'TCP-002' -Category 'TCP' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '200', '/c', '4', '/k', '8', '/z', '1024', '/q', '/stats')
    Invoke-StopTest -TestId 'UDP-001' -Category 'UDP' -Protocol 'udp' -PeerArguments @('127.0.0.1', '/p', 'udp', '/r', '@PORT@', '/n', '50', '/z', '1200', '/q', '/stats')
    Invoke-StopTest -TestId 'UDP-002' -Category 'UDP' -Protocol 'udp' -PeerArguments @('127.0.0.1', '/p', 'udp', '/r', '@PORT@', '/n', '50', '/z', '1', '/q', '/stats')
    Invoke-StopTest -TestId 'CAPACITY-001' -Category 'CAPACITY' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '200', '/c', '16', '/k', '4', '/z', '2048', '/q', '/stats')
    Invoke-StopTest -TestId 'ADMISSION-001' -Category 'ADMISSION' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '100', '/c', '32', '/k', '2', '/z', '512', '/q', '/stats')
    Invoke-StopTest -TestId 'RESET-001' -Category 'RESET' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '20', '/t', '1', '/q', '/stats')
    Invoke-StopTest -TestId 'RESET-002' -Category 'RESET' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '20', '/rc', '1', '/q', '/stats')
    Invoke-StopTest -TestId 'SHUTDOWN-001' -Category 'SHUTDOWN' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', '3', '/q', '/stats')
    Invoke-StopTest -TestId 'NOTIFY-001' -Category 'NOTIFY' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', '3', '/c', '8', '/q', '/stats')
    Invoke-StopTest -TestId 'SOAK-001' -Category 'SOAK' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', $Soak, '/c', '8', '/k', '8', '/z', '1024', '/q', '/stats')
    Invoke-StopTest -TestId 'PERF-TCP-001' -Category 'PERF' -Protocol 'tcp' -PeerArguments @('127.0.0.1', '/p', 'tcp', '/r', '@PORT@', '/n', '0', '/w', $Perf, '/c', '8', '/k', '8', '/z', '4096', '/q', '/stats')
    Invoke-StopTest -TestId 'PERF-UDP-001' -Category 'PERF' -Protocol 'udp' -PeerArguments @('127.0.0.1', '/p', 'udp', '/r', '@PORT@', '/n', '0', '/w', $Perf, '/c', '8', '/z', '1200', '/q', '/stats')
}

Save-Results
$failed = @($Rows | Where-Object { $_.result -eq 'FAIL' })
Write-Host ("verification " + $Project + ": " + $Rows.Count + " cases, " + $failed.Count + " failed")
Write-Host ("results: " + (Join-Path $ResultsDir 'latest.csv'))
if ($failed.Count -ne 0) { Write-Host ("failed: " + (($failed | ForEach-Object { $_.test_id }) -join ', ')) }



