param(
    [Parameter(Mandatory)]
    [string] $DriverPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $DriverPath -PathType Leaf)) {
    throw "client test executable not found: $DriverPath"
}

function Invoke-FaultCase {
    param(
        [Parameter(Mandatory)]
        [string] $Mode,
        [Parameter(Mandatory)]
        [int] $ExpectedExitCode,
        [string] $ExpectedStage = ''
    )

    $artifact = Join-Path (Split-Path -Parent $DriverPath) ('fault-artifacts/' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff') + '-' + [Guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $artifact -Force)
    $errorPath = Join-Path $artifact ($Mode + '.stderr.log')
    $outputPath = Join-Path $artifact ($Mode + '.stdout.log')
    $process = Start-Process -FilePath $DriverPath -ArgumentList @('fault', $Mode) -PassThru -WindowStyle Hidden -RedirectStandardError $errorPath -RedirectStandardOutput $outputPath
    try {
        if (-not $process.WaitForExit(15000)) {
            $process.Kill($true)
            if (-not $process.WaitForExit(5000)) { throw "client fault mode '$Mode' cleanup timeout" }
            throw "client fault mode '$Mode' timeout; artifacts=$artifact"
        }
        if ($process.ExitCode -ne $ExpectedExitCode) {
            throw "client fault mode '$Mode' exited with $($process.ExitCode), expected $ExpectedExitCode; artifacts=$artifact"
        }
        $text = [IO.File]::ReadAllText($errorPath)
        if ($ExpectedStage -ne '' -and $text -notmatch ([regex]::Escape($ExpectedStage) + ' failed: native_error=')) {
            throw "client fault mode '$Mode' did not report stage '$ExpectedStage': $text; artifacts=$artifact"
        }
        if ($ExpectedExitCode -eq 0 -and $text.Length -ne 0) { throw "client positive fault mode '$Mode' wrote stderr: $text" }
    } finally {
        if (-not $process.HasExited) {
            $process.Kill($true)
            if (-not $process.WaitForExit(5000)) { throw "client fault mode '$Mode' final cleanup timeout" }
        }
        $process.Dispose()
    }
}

Invoke-FaultCase -Mode 'normal' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'notify_failure' -ExpectedExitCode 4 -ExpectedStage 'test notify failure'
Invoke-FaultCase -Mode 'notify_duplicate' -ExpectedExitCode 4 -ExpectedStage 'RIONotify duplicate arm'
Invoke-FaultCase -Mode 'notify_provider_failure' -ExpectedExitCode 4 -ExpectedStage 'RIONotify(client provider)'
Invoke-FaultCase -Mode 'notify_provider_duplicate' -ExpectedExitCode 4 -ExpectedStage 'RIONotify duplicate arm'
Invoke-FaultCase -Mode 'notify_precondition_duplicate' -ExpectedExitCode 4 -ExpectedStage 'client notification duplicate arm'
Invoke-FaultCase -Mode 'corrupt_cq' -ExpectedExitCode 4
Invoke-FaultCase -Mode 'zero_latency_frequency' -ExpectedExitCode 4 -ExpectedStage 'client latency frequency'
Invoke-FaultCase -Mode 'resource_deinit_live' -ExpectedExitCode 4 -ExpectedStage 'client worker release precondition'
Invoke-FaultCase -Mode 'resource_deinit_drained' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'transport_deinit_live' -ExpectedExitCode 4 -ExpectedStage 'client worker release precondition'
Invoke-FaultCase -Mode 'transport_deinit_drained' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'transport_deinit_notify' -ExpectedExitCode 4 -ExpectedStage 'client transport resource release precondition'
Invoke-FaultCase -Mode 'completion_extent' -ExpectedExitCode 4 -ExpectedStage 'client completion exceeds posted range'
Invoke-FaultCase -Mode 'send_offset_underflow' -ExpectedExitCode 4 -ExpectedStage 'client send window'
Invoke-FaultCase -Mode 'completion_valid' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'completion_slot_identity' -ExpectedExitCode 4 -ExpectedStage 'client RIO RequestContext'
Invoke-FaultCase -Mode 'completion_socket_context' -ExpectedExitCode 4 -ExpectedStage 'client RIO SocketContext identity'
Invoke-FaultCase -Mode 'completion_stale_generation' -ExpectedExitCode 4 -ExpectedStage 'client completion generation'
Invoke-FaultCase -Mode 'completion_stale_token' -ExpectedExitCode 4 -ExpectedStage 'client completion identity'

Write-Host 'PASS client fail-fast boundaries'
