# Builds and verifies cpp-echo-client through its Visual Studio 2026 solution.
#
# Stages: formatter check, MSBuild of the named-module solution, the four unit suites, the source
# policy, the fail-fast boundaries, the process contract, the quota contract and the reset storm.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration
)

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
# Resolves the Visual Studio installation, MSBuild and the LLVM formatter that ships with it.
function Get-VisualStudio {
    $vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe was not found.' }
    $installation = (& $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
    if (-not $installation) { throw 'A Visual Studio installation with the x64 C++ tools was not found.' }
    return $installation
}

function Get-Formatter {
    param([string] $Installation)
    # The formatter that belongs to the project's own toolset keeps formatting independent of
    # whatever clang-format another toolchain puts on PATH.
    $shipped = Join-Path $Installation 'VC\Tools\Llvm\x64\bin\clang-format.exe'
    if (Test-Path -LiteralPath $shipped -PathType Leaf) { return $shipped }
    $found = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    if ($null -ne $found) { return $found.Source }
    throw 'clang-format.exe was not found.'
}

function Get-BuildSourceSnapshot {
    param([string] $Root)

    $pending = [System.Collections.Generic.Stack[string]]::new()
    $entries = [System.Collections.Generic.List[string]]::new()
    $pending.Push($Root)
    $excluded = @('x64', '.vs', '.git', 'build') | ForEach-Object { [System.IO.Path]::GetFullPath((Join-Path $Root $_)) }
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

function Invoke-Step {
    param([string] $Label, [scriptblock] $Body)
    & $Body
    if ($LASTEXITCODE -ne 0) { throw "$Label failed." }
    Write-Host "PASS $Label"
}

$installation = Get-VisualStudio
$formatter = Get-Formatter -Installation $installation
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) { throw "MSBuild was not found at $msbuild." }

$sourceBefore = Get-BuildSourceSnapshot -Root $projectRoot
try {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests') -File -Recurse -Filter *.cpp) {
        & $formatter --dry-run --Werror --style=file $file.FullName
        if ($LASTEXITCODE -ne 0) { throw "Formatting check failed for $($file.FullName); run format.ps1 explicitly." }
    }

    Write-Host "== MSBuild $Configuration and test configurations"
    foreach ($buildConfiguration in @($Configuration, ($Configuration + 'Tests'))) {
        & $msbuild (Join-Path $projectRoot 'cpp-echo-client.vcxproj') /p:Configuration=$buildConfiguration /p:Platform=x64 /v:minimal /nologo /m /nodeReuse:false
        if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for $buildConfiguration." }
    }

    $output = Join-Path $projectRoot ("x64\" + $Configuration)
    $client = Join-Path $output 'cpp-echo-client.exe'
    $tests = Join-Path (Join-Path $projectRoot ("x64\" + $Configuration + 'Tests')) 'cpp-echo-client-tests.exe'
    foreach ($suite in 'core', 'runtime', 'engine', 'contracts') {
        Invoke-Step -Label "unit suite $suite" -Body { & $tests $suite | Out-Null }
    }
    }
    Invoke-Step -Label 'source policy' -Body {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests/cec_source_policy.ps1') -ProjectRoot $projectRoot
    }
    Invoke-Step -Label 'fail-fast boundaries' -Body {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests/cec_fault_process_tests.ps1') -DriverPath $tests
    }
    Invoke-Step -Label 'process contract' -Body {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests/cec_process_tests.ps1') -ClientPath $client
    }
    Invoke-Step -Label 'quota contract' -Body {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests/cec_quota_process_tests.ps1') -ClientPath $client
    }
    Invoke-Step -Label 'reset storm' -Body {
        & pwsh -NoProfile -File (Join-Path $projectRoot 'tests/cec_reset_storm_tests.ps1') -ClientPath $client
    }
} finally {
    $sourceAfter = Get-BuildSourceSnapshot -Root $projectRoot
    if ($sourceAfter -cne $sourceBefore) { throw 'Build/test modified source file contents.' }
}

Write-Host "PASS cpp-echo-client $Configuration MSBuild, unit suites, source policy, fault boundaries, process, quota and reset storm"
