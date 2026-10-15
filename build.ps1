param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$platform = 'x64'
$project = Join-Path $projectRoot 'cpp-echo-client.vcxproj'
$outputDirectory = Join-Path $projectRoot (Join-Path $platform $Configuration)
$testOutputDirectory = Join-Path $projectRoot (Join-Path $platform ($Configuration + 'Tests'))
$pureOutputDirectory = Join-Path $projectRoot (Join-Path $platform ($Configuration + 'Pure'))
$logDirectory = Join-Path $projectRoot 'logs'

function Get-BuildSourceSnapshot {
    param([string] $Root)

    $pending = [System.Collections.Generic.Stack[string]]::new()
    $entries = [System.Collections.Generic.List[string]]::new()
    $pending.Push($Root)
    $excluded = @('build', '.git', '.build', '.swiftpm', '.vs', 'logs', 'x64') |
        ForEach-Object { [System.IO.Path]::GetFullPath((Join-Path $Root $_)) }
    while ($pending.Count -ne 0) {
        $directory = $pending.Pop()
        foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
            if ($item.FullName -in $excluded) { continue }
            if ($item.PSIsContainer) {
                $pending.Push($item.FullName)
            } else {
                $relative = [System.IO.Path]::GetRelativePath($Root, $item.FullName)
                $digest = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
                $entries.Add("$relative=$digest")
            }
        }
    }
    return (@($entries | Sort-Object) -join [Environment]::NewLine)
}

function Get-VisualStudioPath {
    $vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe was not found.' }
    $installation = (& $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
    if (-not $installation) { throw 'A Visual Studio installation with the x64 C++ tools was not found.' }
    return $installation
}

function Get-MSBuildPath {
    param([string] $Installation)
    $msbuild = Join-Path $Installation 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) { throw "MSBuild was not found at $msbuild." }
    return $msbuild
}

function Get-FormatterPath {
    param([string] $Installation)
    $shipped = Join-Path $Installation 'VC\Tools\Llvm\x64\bin\clang-format.exe'
    if (Test-Path -LiteralPath $shipped -PathType Leaf) { return $shipped }
    $found = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    if ($null -ne $found) { return $found.Source }
    throw 'clang-format.exe was not found.'
}

# Every suite writes one retained log next to the configuration output. A failing gate keeps its log.
function Invoke-Gate {
    param(
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][scriptblock] $Command
    )

    $logPath = Join-Path $logDirectory ("$($Configuration.ToLowerInvariant())-$Name.log")
    Write-Host "== $Name"
    $global:LASTEXITCODE = 0
    & $Command *> $logPath
    $exitCode = $LASTEXITCODE
    $tail = @(Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue | Where-Object { $_ -match '\S' })
    if ($tail.Count -gt 0) { Write-Host $tail[-1] }
    if ($exitCode -ne 0) {
        Get-Content -LiteralPath $logPath | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        throw "$Name failed with exit code $exitCode; full log: $logPath"
    }
}

New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$installation = Get-VisualStudioPath
$msbuild = Get-MSBuildPath -Installation $installation
$formatter = Get-FormatterPath -Installation $installation

$sourceBefore = Get-BuildSourceSnapshot -Root $projectRoot
try {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests') -File -Recurse -Filter *.cpp) {
        & $formatter --dry-run --Werror --style=file $file.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "Formatting check failed for $($file.FullName); run format.ps1 explicitly."
        }
    }

    $buildLog = Join-Path $logDirectory ("$($Configuration.ToLowerInvariant())-msbuild.log")
    foreach ($buildConfiguration in @($Configuration, ($Configuration + 'Tests'), ($Configuration + 'Pure'))) {
        Write-Host "== MSBuild $buildConfiguration/$platform"
        & $msbuild $project /p:Configuration=$buildConfiguration /p:Platform=$platform /m /nologo /v:minimal "/flp:LogFile=$buildLog;Verbosity=normal" /clp:Summary
        if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for $buildConfiguration/$platform; full log: $buildLog" }
    }
    $warnings = @(Select-String -LiteralPath $buildLog -Pattern ': warning|warning MSB|warning LNK|warning C' -ErrorAction SilentlyContinue)
    if ($warnings.Count -ne 0) {
        $warnings | Select-Object -First 10 | ForEach-Object { Write-Host $_.Line }
        throw "MSBuild reported $($warnings.Count) warning(s); /WX builds must stay at zero warnings. Log: $buildLog"
    }

    $testsExe = Join-Path $testOutputDirectory 'cpp-echo-client-tests.exe'
    $pureTestsExe = Join-Path $pureOutputDirectory 'cpp-echo-client-pure-tests.exe'
    $productExe = Join-Path $outputDirectory 'cpp-echo-client.exe'

    Invoke-Gate -Name 'contracts' -Command { & $testsExe contracts }
    Invoke-Gate -Name 'engine' -Command { & $testsExe engine }
    Invoke-Gate -Name 'runtime' -Command { & $testsExe runtime }
    Invoke-Gate -Name 'pure-engine' -Command { & $pureTestsExe pure }
    Invoke-Gate -Name 'fault-boundaries' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\cec_fault_process_tests.ps1') -DriverPath $testsExe
    }
    Invoke-Gate -Name 'source-policy' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\cec_source_policy.ps1') -ProjectRoot $projectRoot
    }
    Invoke-Gate -Name 'process-loopback' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\cec_process_tests.ps1') -ClientPath $productExe
    }
    Invoke-Gate -Name 'process-quota' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\cec_quota_process_tests.ps1') -ClientPath $productExe
    }
    Invoke-Gate -Name 'reset-storm' -Command {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests\cec_reset_storm_tests.ps1') -ClientPath $productExe -Label 'cpp-echo-client'
    }
} finally {
    $sourceAfter = Get-BuildSourceSnapshot -Root $projectRoot
    if ($sourceAfter -cne $sourceBefore) { throw 'Build/test modified source file contents.' }
}

Write-Host "$Configuration/$platform gates passed: MSBuild 0 warnings, unit/contract, engine and runtime suites, pure-engine, fault boundaries, source policy, process loopback, process quota and reset storm."