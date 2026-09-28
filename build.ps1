param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration
)

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'vswhere.exe was not found; install Visual Studio with Desktop development with C++.'
}
$vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    throw 'A Visual Studio installation containing the MSVC x64 toolset was not found.'
}
$devShell = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$formatterCommand = Get-Command clang-format.exe -ErrorAction SilentlyContinue
$formatter = if ($null -ne $formatterCommand) {
    $formatterCommand.Source
} else {
    Join-Path $vsPath 'VC\Tools\Llvm\x64\bin\clang-format.exe'
}
if (-not (Test-Path -LiteralPath $formatter -PathType Leaf)) {
    throw 'clang-format.exe was not found in PATH or the Visual Studio LLVM tools.'
}
$formatFiles = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'include'), (Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests') -File -Recurse |
    Where-Object { $_.Extension -in '.h', '.hpp', '.cpp' }
foreach ($file in $formatFiles) {
    & $formatter -i --style=file $file.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "clang-format failed for $($file.FullName)"
    }
}

$leaf = $Configuration.ToLowerInvariant()
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'build'))
$buildDirectory = [System.IO.Path]::GetFullPath((Join-Path $buildRoot $leaf))
$expectedPrefix = $buildRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
if (-not $buildDirectory.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    [System.IO.Path]::GetFileName($buildDirectory) -ne $leaf) {
    throw "Refusing to clean unexpected build directory: $buildDirectory"
}
if (Test-Path -LiteralPath $buildDirectory) {
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force
}

$runtime = if ($Configuration -eq 'Debug') { 'MultiThreadedDebug' } else { 'MultiThreaded' }
& cmake -S $projectRoot -B $buildDirectory -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_MSVC_RUNTIME_LIBRARY=$runtime"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
& ctest --test-dir $buildDirectory --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
