param(
    [Parameter(Mandatory)]
    [string] $DriverPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $DriverPath -PathType Leaf)) {
    throw "client fault driver not found: $DriverPath"
}

function Invoke-FaultCase {
    param(
        [Parameter(Mandatory)]
        [string] $Mode,
        [Parameter(Mandatory)]
        [int] $ExpectedExitCode
    )

    $process = Start-Process -FilePath $DriverPath -ArgumentList @($Mode) -PassThru -Wait -WindowStyle Hidden
    try {
        if ($process.ExitCode -ne $ExpectedExitCode) {
            throw "client fault mode '$Mode' exited with $($process.ExitCode), expected $ExpectedExitCode"
        }
    } finally {
        $process.Dispose()
    }
}

Invoke-FaultCase -Mode 'normal' -ExpectedExitCode 0
Invoke-FaultCase -Mode 'notify_failure' -ExpectedExitCode 4
Invoke-FaultCase -Mode 'corrupt_cq' -ExpectedExitCode 4
Invoke-FaultCase -Mode 'zero_latency_frequency' -ExpectedExitCode 4

Write-Host 'PASS client fail-fast boundaries'
