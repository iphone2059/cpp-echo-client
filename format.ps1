# Formats every named-module source and test translation unit with the repository style.
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)

$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe was not found.' }
$installation = (& $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
if (-not $installation) { throw 'A Visual Studio installation with the x64 C++ tools was not found.' }
$shipped = Join-Path $installation 'VC\Tools\Llvm\x64\bin\clang-format.exe'
$formatter = if (Test-Path -LiteralPath $shipped -PathType Leaf) { $shipped } else {
    $found = Get-Command clang-format.exe -ErrorAction SilentlyContinue
    if ($null -ne $found) { $found.Source } else { throw 'clang-format.exe was not found.' }
}

$files = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests') -File -Recurse -Filter *.cpp
& $formatter -i --style=file @($files | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { throw 'clang-format failed.' }
Write-Host "PASS clang-format applied to $($files.Count) sources"
