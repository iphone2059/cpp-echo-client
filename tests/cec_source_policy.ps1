param(
    [Parameter(Mandatory)]
    [string] $ProjectRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The implementation is one Visual Studio 2026 project built from C++20 named modules; there is no
# header source and no CMake. The policy checks the module graph, the single-project build index and
# the language constraints.
$cecRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$cecSourceDirectories = @('src', 'tests') | ForEach-Object { Join-Path $cecRoot $_ }
$cecSources = @(Get-ChildItem -LiteralPath $cecSourceDirectories -File -Recurse | Where-Object {
        $_.Extension -in @('.cpp', '.h', '.hpp', '.c', '.cc', '.cxx')
    })
$cecFailures = [System.Collections.Generic.List[string]]::new()

function Add-CecFailure {
    param([string] $Message)
    $cecFailures.Add($Message)
}

function Test-CecPattern {
    param([string] $Pattern, [string] $Label, [System.IO.FileInfo[]] $Files)
    foreach ($file in $Files) {
        foreach ($match in @(Select-String -LiteralPath $file.FullName -Pattern $Pattern -AllMatches)) {
            Add-CecFailure ("{0}: {1}:{2}" -f $Label, $file.FullName, $match.LineNumber)
        }
    }
}

# 1. No header source may exist in the project tree.
$cecHeaders = @(Get-ChildItem -LiteralPath $cecRoot -File -Recurse -Force | Where-Object {
        $_.Extension -in @('.h', '.hpp', '.hxx') -and
        $_.FullName -notmatch '[\\/](x64|\.vs|build|\.build)[\\/]'
    })
foreach ($header in $cecHeaders) {
    Add-CecFailure ("header source present: {0}" -f $header.FullName)
}

# 2. Every production module is one .cpp that declares its module and opens the global module
#    fragment before its includes.
$cecModules = [ordered]@{
    'types'    = 'cec.types'
    'metrics'  = 'cec.metrics'
    'payload'  = 'cec.payload'
    'contract' = 'cec.contract'
    'native'   = 'cec.native'
    'internal' = 'cec.internal'
    'engine'   = 'cec.engine'
}
foreach ($entry in $cecModules.GetEnumerator()) {
    $path = Join-Path $cecRoot ("src\{0}.cpp" -f $entry.Key)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Add-CecFailure ("missing module source: src\{0}.cpp" -f $entry.Key)
        continue
    }
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -notmatch ("(?m)^export module {0};" -f [regex]::Escape($entry.Value))) {
        Add-CecFailure ("src\{0}.cpp must declare 'export module {1};'" -f $entry.Key, $entry.Value)
    }
    if ($text -notmatch '(?m)^module;') {
        Add-CecFailure ("src\{0}.cpp must open the global module fragment before its includes" -f $entry.Key)
    }
}

# 3. The pure modules keep no native or higher-layer dependency.
foreach ($name in @('types', 'metrics', 'internal')) {
    $path = Join-Path $cecRoot ("src\{0}.cpp" -f $name)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $file = @(Get-Item -LiteralPath $path)
    Test-CecPattern -Pattern '(?m)^(export )?import cec\.(native|engine|payload|contract);' -Label 'higher layer import in pure module' -Files $file
    Test-CecPattern -Pattern '#\s*include\s*[<"](Windows|WinSock2|MSWSock|WS2tcpip|mmsystem)(\.h)?[>"]|\b(?:SOCKET|HANDLE|OVERLAPPED|RIO_(?:RQ|CQ|BUF))\b' -Label 'native dependency in pure module' -Files $file
}

# 4. Only the declared production modules plus test support modules may exist.
$cecExporters = @($cecModules.Values)
foreach ($file in @(Get-ChildItem -LiteralPath $cecSourceDirectories -File -Recurse -Filter *.cpp)) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($match in [regex]::Matches($text, '(?m)^export module ([A-Za-z0-9_.]+);')) {
        $declared = $match.Groups[1].Value
        if ($cecExporters -notcontains $declared -and $declared -notlike 'cec.test.*') {
            Add-CecFailure ("unknown module declaration '{0}' in {1}" -f $declared, $file.FullName)
        }
    }
    if ($file.DirectoryName -eq (Join-Path $cecRoot 'tests') -and
        $text -match '(?m)^export module cec\.(types|metrics|payload|contract|native|internal|engine);') {
        Add-CecFailure ("test translation unit re-declares a production module: {0}" -f $file.FullName)
    }
}

# 5. One project builds the implementation: product, test and pure-engine configurations of a
#    single .vcxproj. The pure configuration must stay native free and every source must be
#    listed by the project.
$cecProjects = @(Get-ChildItem -LiteralPath $cecRoot -File -Filter *.vcxproj)
if ($cecProjects.Count -ne 1) {
    Add-CecFailure ("expected exactly one Visual Studio project, found {0}" -f $cecProjects.Count)
}
$cecProjectText = ($cecProjects | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join [Environment]::NewLine
$quote = [char]39
function Get-CecConfigurationMarker {
    param([string] $Configuration, [string] $Kind)
    if ($Kind -eq 'group') {
        return '<ItemGroup Condition="' + $quote + '$(Configuration)|$(Platform)' + $quote + '==' + $quote + $Configuration + '|x64' + $quote + '">'
    }
    return '<ProjectConfiguration Include="' + $Configuration + '|x64">'
}
$cecConfigurations = @('Debug', 'Release', 'DebugTests', 'ReleaseTests', 'DebugPure', 'ReleasePure')
foreach ($configuration in $cecConfigurations) {
    foreach ($kind in @('project', 'group')) {
        $marker = Get-CecConfigurationMarker -Configuration $configuration -Kind $kind
        if ($cecProjectText.IndexOf($marker) -lt 0) {
            Add-CecFailure ("project does not declare the {0} entry for {1}|x64" -f $kind, $configuration)
        }
    }
}
function Get-CecGroupSources {
    param([string] $Text, [string] $Configuration)
    $marker = Get-CecConfigurationMarker -Configuration $Configuration -Kind 'group'
    $start = $Text.IndexOf($marker)
    if ($start -lt 0) { return @() }
    $end = $Text.IndexOf('</ItemGroup>', $start)
    $group = $Text.Substring($start, $end - $start)
    return @([regex]::Matches($group, 'Include="([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
}
$productSources = Get-CecGroupSources -Text $cecProjectText -Configuration 'Release'
$testSources = Get-CecGroupSources -Text $cecProjectText -Configuration 'ReleaseTests'
$pureSources = Get-CecGroupSources -Text $cecProjectText -Configuration 'ReleasePure'
if (@($productSources | Where-Object { $_ -like 'tests\*' }).Count -ne 0) {
    Add-CecFailure 'the product configuration lists test sources'
}
if ($productSources -notcontains 'src\main.cpp') {
    Add-CecFailure 'the product configuration does not list src\main.cpp'
}
if (@($testSources | Where-Object { $_ -eq 'src\main.cpp' }).Count -ne 0) {
    Add-CecFailure 'the test configuration lists the product entry point src\main.cpp'
}
if ($testSources -notcontains 'tests\cec_test_entry.cpp') {
    Add-CecFailure 'the test configuration does not list tests\cec_test_entry.cpp'
}
if ($pureSources -notcontains 'tests\cec_test_entry.cpp' -or $pureSources -notcontains 'tests\cec_pure_engine_tests.cpp') {
    Add-CecFailure 'the pure-engine configuration does not list its entry and suite'
}
if (@($pureSources | Where-Object { $_ -like '*native*' -or $_ -eq 'src\engine.cpp' -or $_ -eq 'src\contract.cpp' -or $_ -eq 'src\payload.cpp' }).Count -ne 0) {
    Add-CecFailure 'the pure-engine configuration lists a native or higher-layer source'
}
if ($cecProjectText -notmatch 'CEC_PURE_ENGINE_ONLY') {
    Add-CecFailure 'no configuration defines CEC_PURE_ENGINE_ONLY'
}
$pureMarker = Get-CecConfigurationMarker -Configuration 'ReleasePure' -Kind 'group'
$pureStart = $cecProjectText.IndexOf($pureMarker)
if ($pureStart -ge 0) {
    $pureEnd = $cecProjectText.IndexOf('</ItemDefinitionGroup>', $pureStart)
    if ($pureEnd -gt $pureStart) {
        $segment = $cecProjectText.Substring($pureStart, $pureEnd - $pureStart)
        if ($segment -match 'ws2_32') { Add-CecFailure 'the pure-engine configuration links ws2_32' }
    }
}
foreach ($file in $cecSources | Where-Object { $_.Extension -eq '.cpp' }) {
    $relative = [System.IO.Path]::GetRelativePath($cecRoot, $file.FullName)
    if ($cecProjectText -notmatch [regex]::Escape($relative)) {
        Add-CecFailure ("source is not listed in the project: {0}" -f $relative)
    }
}
# 6. The former language constraints still hold.
Test-CecPattern -Pattern '\b(namespace|using|throw|try|catch|goto|dynamic_cast|typeid)\b|std::(function|bind|shared_ptr|async)|\.detach\s*\(' -Label 'forbidden C++ construct' -Files $cecSources
Test-CecPattern -Pattern '^\s*#\s*define\b' -Label 'project macro in implementation file' -Files @($cecSources | Where-Object Extension -In @('.c', '.cc', '.cpp', '.cxx'))
Test-CecPattern -Pattern '\b(sendto|recvfrom|WSASend|WSARecv|WSASendTo|WSARecvFrom)\s*\(' -Label 'non-RIO payload API' -Files $cecSources
Test-CecPattern -Pattern '\b(CreateFile|CreateEvent|CreateMutex|CreateSemaphore|LoadLibrary|GetModuleHandle|MessageBox)\s*\(' -Label 'non-Unicode Windows API' -Files $cecSources
Test-CecPattern -Pattern '^\s*#\s*include\s*[<"][^>"]+\.cpp[>"]' -Label 'implementation file inclusion' -Files $cecSources

# The transport API is named send/recv, so a bare verb is only a raw payload call when the line is
# neither a qualified member access nor a member declaration. Only production sources are checked.
function Test-CecPayloadVerb {
    param([System.IO.FileInfo[]] $Files)
    foreach ($file in $Files) {
        $lines = Get-Content -LiteralPath $file.FullName
        for ($index = 0; $index -lt $lines.Count; ++$index) {
            $line = $lines[$index]
            if ($line -notmatch '(?<![.\w>:])(send|recv)\s*\(') { continue }
            if ($line -match '^\s*(?:\[\[noreturn\]\]\s*)?[\w:<>,\s\*&]+?\b(?:send|recv)\s*\(') { continue }
            Add-CecFailure ("non-RIO payload API: {0}:{1}" -f $file.FullName, ($index + 1))
        }
    }
}
Test-CecPayloadVerb -Files @($cecSources | Where-Object {
        $_.Extension -in @('.c', '.cc', '.cpp', '.cxx') -and $_.DirectoryName -eq (Join-Path $cecRoot 'src')
    })

# 7. No CMake build, no reference to another implementation, nothing outside the project root.
Test-CecPattern -Pattern 'CMakeLists\.txt|add_subdirectory\s*\(|cmake_minimum_required' -Label 'CMake build artefact' -Files @($cecProjects + @(Get-Item -LiteralPath (Join-Path $cecRoot 'README.md') -ErrorAction SilentlyContinue))
function Test-CecForeignReference {
    param([System.IO.FileInfo[]] $Files)
    $ownName = Split-Path -Path $cecRoot -Leaf
    foreach ($file in $Files) {
        foreach ($match in @(Select-String -LiteralPath $file.FullName -Pattern '\b[a-z][a-z0-9]*-echo-(?:client|server)\b' -AllMatches)) {
            foreach ($found in $match.Matches) {
                if ($found.Value -ne $ownName) {
                    Add-CecFailure ("foreign implementation reference '{0}': {1}:{2}" -f $found.Value, $file.FullName, $match.LineNumber)
                }
            }
        }
    }
}
$cecDependencyFiles = @($cecProjects + @(Get-ChildItem -LiteralPath $cecRoot -File | Where-Object {
            $_.Extension -in @('.ps1', '.slnx')
        }) + @(Get-ChildItem -LiteralPath (Join-Path $cecRoot 'tests') -File | Where-Object {
            $_.FullName -ne $PSCommandPath -and
            $_.Extension -eq '.ps1'
        }))
Test-CecForeignReference -Files $cecDependencyFiles
Test-CecPattern -Pattern '\.\.[\\/]|add_subdirectory\s*\(|cmake\s+-S' -Label 'cross-project dependency' -Files $cecDependencyFiles

if ($cecFailures.Count -ne 0) {
    $cecFailures | ForEach-Object { Write-Error $_ }
    throw "client source policy failed with $($cecFailures.Count) violation(s)"
}

Write-Host 'PASS client named-module source policy: single project, no header source, layered modules'
