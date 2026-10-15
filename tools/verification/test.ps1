param(
    [string] $Filter = '',
    [int] $SoakSeconds = 10,
    [int] $PerfSeconds = 5,
    [int] $PortBase = 26000,
    [string] $ExecutablePath = '',
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [string] $ArtifactDirectory = '',
    [switch] $GateSelfTest
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
$ResultsDir = if ($ArtifactDirectory) { [System.IO.Path]::GetFullPath($ArtifactDirectory) } else { Join-Path $PSScriptRoot 'results' }
$ArchiveDir = Join-Path $ResultsDir 'archive'
New-Item -ItemType Directory -Force -Path $ArchiveDir | Out-Null
$CaseDir = Join-Path $ResultsDir 'cases'
New-Item -ItemType Directory -Force -Path $CaseDir | Out-Null

$ExecutableFile = Join-Path $PSScriptRoot 'executable.txt'
$FeaturesFile   = Join-Path $PSScriptRoot 'features.txt'

if (-not $ExecutablePath -and -not (Test-Path -LiteralPath $ExecutableFile -PathType Leaf)) { throw "executable.txt is missing: $ExecutableFile" }
$Executable = if ($ExecutablePath) { [System.IO.Path]::GetFullPath($ExecutablePath) } else { [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ((Get-Content -LiteralPath $ExecutableFile -Raw).Trim()))) }
$Features   = @()
if (Test-Path -LiteralPath $FeaturesFile -PathType Leaf) {
    $Features = @(Get-Content -LiteralPath $FeaturesFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
}
$HasFeature = { param([string] $Name) return $Features -contains $Name }

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "executable not found: $Executable - edit the text file next to this script"
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
    'notify_deliveries','notify_gap','notify_timeouts','stdout_valid','stderr_empty',
    'artifact_directory','message')
$Rows = [System.Collections.Generic.List[object]]::new()
$Script:Port = $PortBase

function Get-NextPort { $Script:Port = $Script:Port + 1; return $Script:Port }

function Test-CaseSelected {
    param([string] $TestId)
    if (-not $Filter) { return $true }
    foreach ($pattern in ($Filter -split ',')) {
        $pattern = $pattern.Trim()
        if ($pattern -and $TestId -like $pattern) { return $true }
    }
    return $false
}

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
    $row['build_type']       = $Configuration.ToLowerInvariant()
    foreach ($key in $Values.Keys) { if ($row.Contains($key)) { $row[$key] = $Values[$key] } }
    $Rows.Add([pscustomobject]$row)
    Write-Host ("{0,-8} {1,-16} {2}" -f $row['result'], $row['test_id'], $row['message'])
}

function Convert-FinalLine {
    param([string] $Text, [string] $Role, [bool] $Quiet = $true)
    $fields = @{ _error = '' }
    $outputLines = @($Text -split "\r?\n" | Where-Object { $_.Trim() -ne '' })
    $lines = @($outputLines | Where-Object { $_ -match '^\s*final(?:\s|$)' })
    if ($lines.Count -ne 1) { $fields._error = "expected exactly one final line, saw $($lines.Count)"; return $fields }
    $integer = '(?:0|[1-9][0-9]*)'
    $decimal = '(?:0|[1-9][0-9]*)\.[0-9]{2}'
    if ($Role -eq 'client') {
        $schema = [ordered]@{ elapsed_ms=$integer; sessions=$integer; active=$integer; attempted=$integer;
            pending=$integer; echoed=$integer; corrupted=$integer; lost=$integer; cancelled=$integer;
            sent_bytes=$integer; received_bytes=$integer; bytes=$integer; connections=$integer;
            reconnects=$integer; network_errors=$integer; echo_per_sec=$decimal; MiB_per_sec=$decimal;
            p50_us=$integer; p99_us=$integer; p999_us=$integer; mean_us=$integer; max_us=$integer;
            latency_sample='batch' }
    } else {
        $schema = [ordered]@{ protocol='(?:tcp|udp)'; elapsed_ms=$integer; workers=$integer;
            accepted=$integer; active=$integer; outstanding=$integer; completions=$integer;
            receives=$integer; sends=$integer; received_bytes=$integer; sent_bytes=$integer;
            bytes=$integer; network_errors=$integer; rejected=$integer; MiB_per_sec=$decimal }
    }
    $parts = foreach ($key in $schema.Keys) {
        $separator = if ($key -in @('p50_us','p99_us','p999_us','max_us')) { '~' } else { '=' }
        '(?<' + $key + '>' + [regex]::Escape($key) + $separator + $schema[$key] + ')'
    }
    $match = [regex]::Match($lines[0], '^final ' + ($parts -join ' ') + '$')
    if (-not $match.Success) { $fields._error = 'final schema, field order, type or separator differs'; return $fields }
    foreach ($line in $outputLines) {
        if ($line -ceq $lines[0]) { continue }
        if ($Quiet -or $Role -ne 'client' -or $line -notmatch '^report ') {
            $fields._error = 'unexpected stdout line'; return $fields
        }
        $report = [regex]::Match($line, '^report ' + ($parts -join ' ') + '$')
        if (-not $report.Success) { $fields._error = 'report schema, field order, type or separator differs'; return $fields }
        foreach ($key in $schema.Keys) {
            if ($schema[$key] -ne $integer) { continue }
            $value = $report.Groups[$key].Value -replace ('^' + [regex]::Escape($key) + '[=~]'), ''
            $parsed = [uint64]0
            if (-not [uint64]::TryParse($value, [ref]$parsed)) { $fields._error = "report $key exceeds uint64"; return $fields }
        }
    }
    foreach ($key in $schema.Keys) {
        $value = $match.Groups[$key].Value -replace ('^' + [regex]::Escape($key) + '[=~]'), ''
        if ($schema[$key] -eq $integer) {
            $parsed = [uint64]0
            if (-not [uint64]::TryParse($value, [ref]$parsed)) { $fields._error = "final $key exceeds uint64"; return $fields }
        }
        $fields[$key] = $value
    }
    if ($Role -eq 'client') {
        if ($fields.active -ne '0' -or $fields.pending -ne '0') { $fields._error = 'client terminal active or pending is nonzero' }
        $total = [System.Numerics.BigInteger]::Parse($fields.echoed) + [System.Numerics.BigInteger]::Parse($fields.corrupted) +
            [System.Numerics.BigInteger]::Parse($fields.lost) + [System.Numerics.BigInteger]::Parse($fields.cancelled)
        if ($total -ne [System.Numerics.BigInteger]::Parse($fields.attempted)) { $fields._error = 'client attempted does not equal terminal outcomes' }
    } elseif ($fields.active -ne '0' -or $fields.outstanding -ne '0') {
        $fields._error = 'server terminal active or outstanding is nonzero'
    }
    return $fields
}

function Get-WorkerCount {
    param([string] $Role, [string] $Protocol, [string[]] $Arguments)
    $requested = 0; $sessions = 1
    for ($i = 0; $i -lt $Arguments.Count - 1; $i++) {
        if ($Arguments[$i] -eq '/threads') { $requested = [int]$Arguments[$i + 1] }
        if ($Arguments[$i] -eq '/c') { $sessions = [int]$Arguments[$i + 1] }
    }
    if ($Role -eq 'server' -and $Protocol -eq 'udp') { return 1 }
    if ($requested -eq 0) {
        if (-not ('EchoVerification.Processors' -as [type])) {
            Add-Type -TypeDefinition 'namespace EchoVerification { public static class Processors { [System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint GetActiveProcessorCount(ushort groupNumber); } }'
        }
        $requested = [int][Math]::Min([EchoVerification.Processors]::GetActiveProcessorCount([uint16]65535), 64)
        if ($requested -lt 1) { $requested = 1 }
    }
    if ($Role -eq 'client') { return [Math]::Min($requested, $sessions) }
    return $requested
}

function Convert-Diagnostics {
    param([string] $Path, [string] $Role, [string] $Protocol, [int] $ExpectedWorkers)
    $totals = @{ arms = ''; deliveries = ''; gap = ''; timeouts = '' }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "diagnostics missing: $Path" }
    $lines = @(Get-Content -LiteralPath $Path)
    if ($lines.Count -ne $ExpectedWorkers) { throw "diagnostics: expected $ExpectedWorkers worker lines, saw $($lines.Count)" }
    $workers = [System.Collections.Generic.HashSet[int]]::new()
    $arms = [System.Numerics.BigInteger]::Zero; $deliveries = $arms; $gap = $arms; $timeouts = $arms
    $pattern = '^role=' + $Role + ' protocol=' + $Protocol + ' worker=([0-9]+) notify_arms=([0-9]+) notify_deliveries=([0-9]+) notify_gap=([0-9]+) timeout_wakeups_while_outstanding=([0-9]+)$'
    foreach ($line in $lines) {
        $match = [regex]::Match($line, $pattern)
        if (-not $match.Success) { throw "diagnostics: malformed line: $line" }
        $worker = 0
        if (-not [int]::TryParse($match.Groups[1].Value, [ref]$worker) -or $worker -ge $ExpectedWorkers -or -not $workers.Add($worker)) {
            throw 'diagnostics: duplicate or invalid worker index'
        }
        $values = @()
        for ($i = 2; $i -le 5; $i++) {
            $parsed = [uint64]0
            if (-not [uint64]::TryParse($match.Groups[$i].Value, [ref]$parsed)) { throw 'diagnostics: counter exceeds uint64' }
            $values += [System.Numerics.BigInteger]$parsed
        }
        $difference = $values[0] - $values[1]
        if ($difference -lt 0 -or $difference -gt 1 -or
            $values[2] -ne ($values[0] - $values[1]) -or $values[3] -ne 0) {
            throw 'diagnostics: notification accounting or timeout wakeups invalid'
        }
        $arms += $values[0]; $deliveries += $values[1]; $gap += $values[2]; $timeouts += $values[3]
    }
    return @{ arms = [string]$arms; deliveries = [string]$deliveries; gap = [string]$gap; timeouts = [string]$timeouts }
}

function Test-ExpectedPattern {
    param([string] $Text, [string] $Pattern)
    if (-not $Pattern) { return $true }
    $bounded = [regex]::Replace($Pattern, '[0-9]+', '${0}(?![0-9])')
    $bounded = [regex]::Replace($bounded, '([A-Za-z_][A-Za-z_0-9]*)([=~])', '(?<![A-Za-z0-9_])${1}${2}')
    return [regex]::IsMatch($Text, $bounded)
}

function Invoke-GateSelfTest {
    $clientLine = 'final elapsed_ms=1 sessions=1 active=0 attempted=1 pending=0 echoed=1 corrupted=0 lost=0 cancelled=0 sent_bytes=1 received_bytes=1 bytes=1 connections=1 reconnects=0 network_errors=0 echo_per_sec=1.00 MiB_per_sec=0.00 p50_us~1 p99_us~1 p999_us~1 mean_us=1 max_us~1 latency_sample=batch'
    $serverLine = 'final protocol=tcp elapsed_ms=1 workers=1 accepted=1 active=0 outstanding=0 completions=2 receives=1 sends=1 received_bytes=1 sent_bytes=1 bytes=1 network_errors=0 rejected=0 MiB_per_sec=0.00'
    if ((Convert-FinalLine -Text $clientLine -Role 'client')._error -or (Convert-FinalLine -Text $serverLine -Role 'server')._error) { throw 'gate self-test: valid final rejected' }
    $reportLine = ($clientLine -replace '^final ', 'report ' -replace 'active=0 ', 'active=1 ' -replace 'attempted=1 ', 'attempted=2 ' -replace 'pending=0 ', 'pending=1 ')
    if ((Convert-FinalLine -Text ($reportLine + "`n" + $clientLine) -Role 'client' -Quiet:$false)._error) { throw 'gate self-test: valid periodic report rejected' }
    foreach ($invalid in @(
        ($clientLine + "`n" + $clientLine),
        ($clientLine + "`nextra output"),
        ("extra output`n" + $clientLine),
        ($clientLine -replace 'echoed=1 ', 'echoed=2 '),
        ($clientLine -replace 'pending=0 ', 'pending=1 '),
        ($clientLine -replace 'p50_us~1 ', 'p50_us=1 '),
        ($clientLine + ' extra=1'),
        ($clientLine -replace 'attempted=1 ', 'attempted=18446744073709551616 ')
    )) { if (-not (Convert-FinalLine -Text $invalid -Role 'client')._error) { throw 'gate self-test: invalid client final accepted' } }
    if (-not (Convert-FinalLine -Text ($reportLine + "`n" + $clientLine) -Role 'client' -Quiet:$true)._error) { throw 'gate self-test: quiet report accepted' }
    if (-not (Convert-FinalLine -Text (($reportLine -replace 'active=1 ', 'active=x ') + "`n" + $clientLine) -Role 'client' -Quiet:$false)._error) { throw 'gate self-test: malformed report accepted' }
    if (-not (Convert-FinalLine -Text ($serverLine -replace 'outstanding=0 ', 'outstanding=1 ') -Role 'server')._error) { throw 'gate self-test: outstanding server accepted' }
    if (Test-ExpectedPattern -Text ($clientLine -replace 'echoed=1 ', 'echoed=100 ') -Pattern 'echoed=1') { throw 'gate self-test: integer prefix matched' }
    $path = Join-Path $CaseDir ('gate-selftest-' + [Guid]::NewGuid().ToString('N') + '.diag')
    try {
        Set-Content -LiteralPath $path -Value 'role=client protocol=tcp worker=0 notify_arms=2 notify_deliveries=1 notify_gap=1 timeout_wakeups_while_outstanding=0'
        [void](Convert-Diagnostics -Path $path -Role 'client' -Protocol 'tcp' -ExpectedWorkers 1)
        foreach ($bad in @(
            'role=client protocol=tcp worker=0 notify_arms=2 notify_deliveries=0 notify_gap=2 timeout_wakeups_while_outstanding=0',
            'role=client protocol=tcp worker=0 notify_arms=2 notify_deliveries=1 notify_gap=0 timeout_wakeups_while_outstanding=0',
            'role=client protocol=tcp worker=0 notify_arms=2 notify_deliveries=1 notify_gap=1 timeout_wakeups_while_outstanding=1'
        )) {
            Set-Content -LiteralPath $path -Value $bad
            $rejected = $false
            try { [void](Convert-Diagnostics -Path $path -Role 'client' -Protocol 'tcp' -ExpectedWorkers 1) } catch { $rejected = $true }
            if (-not $rejected) { throw 'gate self-test: invalid diagnostics accepted' }
        }
    } finally { Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue }
    Write-Host 'gate parser self-test passed'
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
        [int] $TimeoutMs = 40000
    )
    if (-not (Test-CaseSelected -TestId $TestId)) { return }
    $port = Get-FreePort
    $tag = [Guid]::NewGuid().ToString('N')
    $casePath = Join-Path $CaseDir ($TestId + '-' + $tag)
    New-Item -ItemType Directory -Path $casePath | Out-Null
    $outFile = Join-Path $casePath 'stdout.txt'
    $errFile = Join-Path $casePath 'stderr.txt'
    $diagFile = Join-Path $casePath 'diagnostics.txt'
    $arguments = @($Arguments | ForEach-Object { $_ -replace '@PORT@', [string]$port })
    $runtime = $arguments -contains '/stats'
    if ($runtime -and $arguments -notcontains '/w') { $arguments += @('/w', '20') }
    $started = Get-Date
    $process = $null
    $finished = $false
    $exit = -1
    $stdout = ''
    $stderr = ''
    $metrics = @{}
    $notify = @{ arms = ''; deliveries = ''; gap = ''; timeouts = '' }
    $failure = ''
    try {
        if ($Component -eq 'client' -and (Test-Feature 'notify-diagnostics')) { $env:CEC_DIAG_FILE = $diagFile }
        if ($Component -eq 'server' -and (Test-Feature 'notify-diagnostics')) { $env:CES_DIAG_FILE = $diagFile }
        $quoted = @($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } })
        $startParams = @{ FilePath=$Executable; RedirectStandardOutput=$outFile;
            RedirectStandardError=$errFile; PassThru=$true; NoNewWindow=$true }
        if ($quoted.Count -gt 0) { $startParams.ArgumentList = $quoted }
        $process = Start-Process @startParams
        $finished = $process.WaitForExit($TimeoutMs)
        if ($finished) { $exit = $process.ExitCode } else { $process.Kill($true) | Out-Null }
        $stdout = Read-TextFile -Path $outFile
        $stderr = Read-TextFile -Path $errFile
        if ($runtime) { $metrics = Convert-FinalLine -Text $stdout -Role $Component -Quiet:($arguments -contains '/q') }
        if ($runtime -and (Test-Feature 'notify-diagnostics')) {
            $workers = Get-WorkerCount -Role $Component -Protocol $Protocol -Arguments $arguments
            $notify = Convert-Diagnostics -Path $diagFile -Role $Component -Protocol $Protocol -ExpectedWorkers $workers
        }
    } catch {
        $failure = $_.Exception.Message
    } finally {
        Remove-Item Env:CEC_DIAG_FILE -ErrorAction SilentlyContinue
        Remove-Item Env:CES_DIAG_FILE -ErrorAction SilentlyContinue
        $duration = [int]((Get-Date) - $started).TotalMilliseconds
        $result = 'FAIL'
        $message = ''
        if ($failure -ne '') { $message = 'case error: ' + $failure }
        elseif (-not $finished) { $message = 'timeout after ' + $TimeoutMs + ' ms' }
        elseif ($exit -ne $ExpectedExit -and ($AllowExitCodes -notcontains $exit)) { $message = 'exit ' + $exit + ', expected ' + $ExpectedExit }
        elseif ($runtime -and $metrics._error) { $message = $metrics._error }
        elseif ($runtime -and $Component -eq 'client' -and $metrics.corrupted -ne '0') { $message = 'client reported corrupted echoes' }
        elseif ($ExpectPattern -ne '' -and -not (Test-ExpectedPattern -Text ($stdout + [char]10 + $stderr) -Pattern $ExpectPattern)) { $message = 'output does not match ' + $ExpectPattern }
        elseif ($ExpectStream -eq 'stderr' -and $stderr.Trim() -eq '') { $message = 'expected the diagnostic on stderr, got none' }
        elseif ($ExpectStream -eq 'stderr' -and $stdout.Trim() -ne '') { $message = 'expected the diagnostic on stderr, got it on stdout' }
        elseif ($ExpectStream -eq 'stdout' -and $stdout.Trim() -eq '') { $message = 'expected the message on stdout, got none' }
        elseif (-not $runtime -and $ExpectedExit -eq 1 -and ($stderr.Trim() -eq '' -or $stdout.Trim() -ne '')) { $message = 'CLI error must be only on stderr' }
        elseif ($runtime -and $exit -eq 3 -and $stderr.Trim() -ne '' -and
            @($stderr.Trim() -split "\r?\n" | Where-Object { $_ -notmatch '^.+ failed: native_error=[0-9]+$' }).Count -gt 0) {
            $message = 'unexpected stderr for runtime failure'
        }
        elseif (($exit -eq 0 -or ($runtime -and $exit -ne 3)) -and $stderr.Trim() -ne '') { $message = 'stderr: ' + $stderr.Trim() }
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
            mib_per_sec = $metrics['MiB_per_sec']; p50_us = $metrics['p50_us']; p99_us = $metrics['p99_us']
            p999_us = $metrics['p999_us']; notify_arms = $notify.arms; notify_deliveries = $notify.deliveries
            notify_gap = $notify.gap; notify_timeouts = $notify.timeouts
            stdout_valid = ($(if ($runtime) { -not $metrics._error } elseif ($ExpectedExit -eq 1) { $stdout.Trim() -eq '' } else { $stdout -match 'Usage:' })).ToString().ToLower()
            stderr_empty = ($stderr.Trim() -eq '').ToString().ToLower()
            artifact_directory = $casePath; message = $message
        }
        [pscustomobject]@{ test_id=$TestId; protocol=$Protocol; configuration=$Configuration;
            port=$port; exit_code=$exit; result=$result } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $casePath 'run.json') -Encoding utf8
    }
}

if ($GateSelfTest) { Invoke-GateSelfTest; exit 0 }

$PortBase = $PortBase
$Soak = [string]$SoakSeconds
$Perf = [string]$PerfSeconds

$CliOne = if ($Component -eq 'client') { '/p' } else { '/p' }
# Artifact writes remain terminating errors; each case already records caught runtime failures.
if ($Component -eq 'client') {
    Invoke-Test -TestId 'CLI-001' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h') -ExpectedExit 0 -ExpectPattern 'Usage:'
    Invoke-Test -TestId 'CLI-002' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/p', 'sctp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments'
    Invoke-Test -TestId 'CLI-003' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/b', '1.0') -ExpectedExit 1
    Invoke-Test -TestId 'CLI-004' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/unknown') -ExpectedExit 1
    Invoke-Test -TestId 'PARSE-001' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/h', '/p', 'udp', '/k', '4') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PARSE-002' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/h', '/p', 'tcp', '/s', '1234') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unknown-switch' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PARSE-003' -Category 'PARSE' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/k', '0') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: out-of-range' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PARSE-004' -Category 'PARSE' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/k', '65537') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: out-of-range' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PARSE-005' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/zzz') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unknown-switch' -ExpectStream 'stderr'
    Invoke-Test -TestId 'ORDER-001' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/k', '4', '/p', 'udp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    Invoke-Test -TestId 'ORDER-002' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/p', 'udp', '/k', '4') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    Invoke-Test -TestId 'ORDER-003' -Category 'ORDER' -Protocol 'tcp' -Arguments @('127.0.0.1', '/k', '4', '/p', 'udp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    Invoke-Test -TestId 'ORDER-004' -Category 'ORDER' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'udp', '/k', '4') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    # Missing arguments are part of the contract: the reference reports the missing target first and
    # the missing protocol second, each with one exact token.
    Invoke-Test -TestId 'MISSING-001' -Category 'MISSING' -Protocol 'tcp' -Arguments @() -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-target' -ExpectStream 'stderr'
    Invoke-Test -TestId 'MISSING-002' -Category 'MISSING' -Protocol 'tcp' -Arguments @('127.0.0.1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-protocol' -ExpectStream 'stderr'
    Invoke-Test -TestId 'MISSING-003' -Category 'MISSING' -Protocol 'tcp' -Arguments @('/p', 'tcp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-target' -ExpectStream 'stderr'
    Invoke-Test -TestId 'HELP-001' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/n', '1', '/d', 'x') -ExpectedExit 0 -ExpectPattern 'Usage:' -ExpectStream 'stdout'
    Invoke-Test -TestId 'HELP-002' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/b', '1.0') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: invalid-number' -ExpectStream 'stderr'
    Invoke-Test -TestId 'HELP-003' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/p', 'tcp', '/rc', '1', '/l', '127.0.0.1:1234') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: invalid-number' -ExpectStream 'stderr'
    # Every line below is a measured reference answer for that command line. The token, the exit code
    # and the stream are the contract, so a paraphrase or a different precedence is a failure.
    Invoke-Test -TestId 'CONFLICT-001' -Category 'CONFLICT' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/d', 'x', '/z', '8') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: conflicting-payload' -ExpectStream 'stderr'
    Invoke-Test -TestId 'CONFLICT-002' -Category 'CONFLICT' -Protocol 'tcp' -Arguments @('/h', '/d', 'x', '/z', '8') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: conflicting-payload' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PORT-001' -Category 'PORT' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/l', '7000', '/c', '2') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: local-port-conflict' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PORT-002' -Category 'PORT' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/l', '7000', '/rc', '1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: local-port-conflict' -ExpectStream 'stderr'
    # The worker split is validated before the mandatory arguments, so a missing target does not win.
    Invoke-Test -TestId 'WORKER-001' -Category 'WORKER' -Protocol 'tcp' -Arguments @('/c', '1', '/threads', '2', '/p', 'tcp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: out-of-range' -ExpectStream 'stderr'
    Invoke-Test -TestId 'QUOTA-005' -Category 'QUOTA' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/n', '18446744073709551615', '/c', '1048576') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: quota-overflow' -ExpectStream 'stderr'
    Invoke-Test -TestId 'VALUE-001' -Category 'VALUE' -Protocol 'tcp' -Arguments @('/q=1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unexpected-value' -ExpectStream 'stderr'
    Invoke-Test -TestId 'VALUE-002' -Category 'VALUE' -Protocol 'tcp' -Arguments @('/r=') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-value' -ExpectStream 'stderr'
    Invoke-Test -TestId 'VALUE-003' -Category 'VALUE' -Protocol 'tcp' -Arguments @('/p', 'sctp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: out-of-range' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PAYLOAD-002' -Category 'PAYLOAD' -Protocol 'udp' -Arguments @('127.0.0.1', '/p', 'udp', '/z', '65508') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: payload-size' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PAYLOAD-003' -Category 'PAYLOAD' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/k', '65536', '/z', '65536') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: payload-size' -ExpectStream 'stderr'
    # /h suppresses only missing-target and missing-protocol; the budgets still apply.
    Invoke-Test -TestId 'CQ-002' -Category 'CQ' -Protocol 'tcp' -Arguments @('/h', '/p', 'tcp', '/c', '200', '/threads', '4', '/cq', '64') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: cq-capacity' -ExpectStream 'stderr'
    # A datagram may be dropped by the host under load, and the reference classifies that as one lost
    # echo plus an echo failure. Out of twenty echoes one loss is tolerated so the case still measures
    # the payload path instead of the scheduler, while corruption and a second loss are not.
    # A one-byte datagram is the smallest legal payload. On a loaded host a single datagram can be lost,
    # and the reference classifies that as lost plus an echo failure, so exit 3 is tolerated here while
    # corruption never is.
    # The default payload is part of the contract: 'echo from <host>' is 19 bytes for 127.0.0.1, so two
    # echoes account for 38 bytes. Every port must put the same bytes on the wire, and this is the only
    # case that pins the default payload length instead of the echo count.
    # /n is a per-session quota: four sessions of two echoes are eight echoes in total. A run that
    # spends one global budget reports 2 instead, which is how Zig and Swift once differed here.
    # One attempt is one receive plus one send whatever /k is, so the largest shard reserves two
    # operations per session against the completion queue and two batches of registered memory.
    Invoke-Test -TestId 'CQ-001' -Category 'CQ' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/c', '200', '/threads', '4', '/cq', '64') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: cq-capacity' -ExpectStream 'stderr'
    Invoke-Test -TestId 'MEM-001' -Category 'MEM' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/k', '2', '/z', '300000', '/memory', '1048576') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: memory-capacity' -ExpectStream 'stderr'
    # The same budgets accepted: the run reaches the network and fails to connect, which is exit 3 and
    # proves the configuration was not rejected.
    Invoke-Test -TestId 'CQ-003' -Category 'CQ' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/c', '64', '/threads', '2', '/cq', '64', '/k', '1', '/t', '1', '/q', '/stats') -ExpectedExit 3
    Invoke-Test -TestId 'CQ-004' -Category 'CQ' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/c', '1', '/k', '64', '/cq', '64', '/z', '1', '/t', '1', '/q', '/stats') -ExpectedExit 3
    Invoke-Test -TestId 'MEM-002' -Category 'MEM' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/k', '2', '/z', '300000', '/memory', '2097152', '/t', '1', '/q', '/stats') -ExpectedExit 3
    # /k is the number of echoes one attempt carries, so a finite quota ends with a shorter attempt and
    # the byte count stays exactly /n times the payload.
    Invoke-Test -TestId 'TIMEOUT-001' -Category 'TIMEOUT' -Protocol 'tcp' -Arguments @('127.0.0.1', '/p', 'tcp', '/r', '9', '/n', '1', '/t', '1', '/w', '5', '/q', '/stats') -ExpectedExit 3
    # Five seconds of unlimited datagram traffic on eight sessions moves a few hundred thousand
    # datagrams; a single drop is the host, not the implementation, so the echo-failure exit code is
    # tolerated while the case still requires real traffic and no corruption.
} else {
    Invoke-Test -TestId 'CLI-001' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h') -ExpectedExit 0 -ExpectPattern 'Usage:'
    Invoke-Test -TestId 'CLI-002' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/p', 'sctp') -ExpectedExit 1
    Invoke-Test -TestId 'CLI-003' -Category 'CLI' -Protocol 'tcp' -Arguments @('/h', '/b', '1.0') -ExpectedExit 1
    Invoke-Test -TestId 'PARSE-001' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/h', '/p', 'udp', '/t', '1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    Invoke-Test -TestId 'PARSE-002' -Category 'PARSE' -Protocol 'tcp' -Arguments @('/zzz') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unknown-switch' -ExpectStream 'stderr'
    # The raw UDP arena fits 1,050,416 bytes, but VirtualAlloc rounds it to 1,052,672.
    # Validation precedes /h; the exact rounded budget must reach Usage.
    Invoke-Test -TestId 'UDP-MEM-001' -Category 'MEM' -Protocol 'udp' -Arguments @('/h', '/p', 'udp', '/k', '16', '/memory', '1050416') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: memory-capacity' -ExpectStream 'stderr'
    Invoke-Test -TestId 'UDP-MEM-002' -Category 'MEM' -Protocol 'udp' -Arguments @('/h', '/p', 'udp', '/k', '16', '/memory', '1052671') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: memory-capacity' -ExpectStream 'stderr'
    Invoke-Test -TestId 'UDP-MEM-003' -Category 'MEM' -Protocol 'udp' -Arguments @('/h', '/p', 'udp', '/k', '16', '/memory', '1052672') -ExpectedExit 0 -ExpectPattern 'Usage:' -ExpectStream 'stdout'
    Invoke-Test -TestId 'UDP-MEM-004' -Category 'MEM' -Protocol 'udp' -Arguments @('/h', '/p', 'udp', '/k', '4097', '/cq', '16384', '/rio-buffer', '1048176', '/memory', '4294967296') -ExpectedExit 0 -ExpectPattern 'Usage:' -ExpectStream 'stdout'
    Invoke-Test -TestId 'ORDER-001' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/t', '1', '/p', 'udp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    Invoke-Test -TestId 'ORDER-002' -Category 'ORDER' -Protocol 'tcp' -Arguments @('/h', '/k', '4', '/p', 'tcp') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: protocol-option' -ExpectStream 'stderr'
    # Missing arguments are part of the contract: the reference reports the missing protocol first and
    # rejects a stray target second.
    Invoke-Test -TestId 'MISSING-001' -Category 'MISSING' -Protocol 'tcp' -Arguments @() -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-protocol' -ExpectStream 'stderr'
    Invoke-Test -TestId 'MISSING-002' -Category 'MISSING' -Protocol 'tcp' -Arguments @('/s', '7000') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: missing-protocol' -ExpectStream 'stderr'
    Invoke-Test -TestId 'MISSING-003' -Category 'MISSING' -Protocol 'tcp' -Arguments @('127.0.0.1') -ExpectedExit 1 -ExpectPattern 'Invalid arguments: unexpected-target' -ExpectStream 'stderr'
    Invoke-Test -TestId 'HELP-001' -Category 'HELP' -Protocol 'tcp' -Arguments @('/h', '/p', 'tcp', '/s', '1234') -ExpectedExit 0 -ExpectPattern 'Usage:' -ExpectStream 'stdout'
}

Save-Results
$failed = @($Rows | Where-Object { $_.result -eq 'FAIL' })
Write-Host ("verification " + $Project + ": " + $Rows.Count + " cases, " + $failed.Count + " failed")
Write-Host ("results: " + (Join-Path $ResultsDir 'latest.csv'))
if ($failed.Count -ne 0) { Write-Host ("failed: " + (($failed | ForEach-Object { $_.test_id }) -join ', ')) }
# A filter that matched nothing is not a green run: it means the run proved nothing.
if ($Rows.Count -eq 0) { Write-Host "no cases matched the filter"; exit 1 }
# The exit code is the machine-readable verdict; a run with failures must not look like success.
if ($failed.Count -ne 0) { exit 1 }
exit 0
