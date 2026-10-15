param(
    [Parameter(Mandatory)]
    [string] $ProjectRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The project is a C++20 named-module project: src holds one .cpp per module and tests hold the
# module consumers. There is no header source at all, so the policy checks the module graph and
# the build index instead of include lines.
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
        $matches = @(Select-String -LiteralPath $file.FullName -Pattern $Pattern -AllMatches)
        foreach ($match in $matches) {
            Add-CecFailure ("{0}: {1}:{2}" -f $Label, $file.FullName, $match.LineNumber)
        }
    }
}

# 1. No header source may exist in the project tree; the conversion removed every one of them.
$cecHeaders = @(Get-ChildItem -LiteralPath $cecRoot -File -Recurse -Force | Where-Object {
        $_.Extension -in @('.h', '.hpp', '.hxx') -and
        $_.FullName -notmatch '[\/](x64|.vs|build|.build)[\/]'
    })
foreach ($header in $cecHeaders) {
    Add-CecFailure ("header source present: {0}" -f $header.FullName)
}

# 2. Every production module is one .cpp that declares its module and exports the surface the
#    other modules and the tests consume.
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

# 3. The pure modules keep no native or higher-layer dependency, exactly as the layered
#    headers did before the conversion.
$cecPureModules = @('types', 'metrics', 'internal')
foreach ($name in $cecPureModules) {
    $path = Join-Path $cecRoot ("src\{0}.cpp" -f $name)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    Test-CecPattern -Pattern '(?m)^(export )?import cec\.(native|engine|payload|contract);' -Label 'higher layer import in pure module' -Files @(Get-Item -LiteralPath $path)
    Test-CecPattern -Pattern '#\s*include\s*[<"](Windows|WinSock2|MSWSock|WS2tcpip|mmsystem)(\.h)?[>"]|\b(?:SOCKET|HANDLE|OVERLAPPED|RIO_(?:RQ|CQ|BUF))\b' -Label 'native dependency in pure module' -Files @(Get-Item -LiteralPath $path)
}

# 4. Only a module interface unit may export, and only the modules listed above may exist.
$cecExporters = @($cecModules.Values)
foreach ($file in @(Get-ChildItem -LiteralPath $cecSourceDirectories -File -Recurse -Filter *.cpp)) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($match in [regex]::Matches($text, '(?m)^export module ([A-Za-z0-9_.]+);')) {
        $declared = $match.Groups[1].Value
        if ($cecExporters -notcontains $declared -and $declared -notlike 'cec.test.*') {
            Add-CecFailure ("unknown module declaration '{0}' in {1}" -f $match.Groups[1].Value, $file.FullName)
        }
    }
    if ($file.DirectoryName -eq (Join-Path $cecRoot 'tests') -and $text -match '(?m)^export module cec\.(types|metrics|payload|contract|native|internal|engine);') {
        Add-CecFailure ("test translation unit re-declares a production module: {0}" -f $file.FullName)
    }
}

# 5. Every module source and every test translation unit is listed in a Visual Studio project, so
#    the solution is the complete build index.
$cecProjects = @(Get-ChildItem -LiteralPath $cecRoot -File -Filter *.vcxproj)
if ($cecProjects.Count -eq 0) {
    Add-CecFailure 'no Visual Studio project file in the project root'
}
$cecProjectText = ($cecProjects | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
foreach ($file in $cecSources | Where-Object { $_.Extension -eq '.cpp' }) {
    $relative = [System.IO.Path]::GetRelativePath($cecRoot, $file.FullName)
    if ($cecProjectText -notmatch [regex]::Escape($relative)) {
        Add-CecFailure ("source is not listed in any project: {0}" -f $relative)
    }
}
foreach ($badge in 'Debug|x64', 'Release|x64') {
    if ($cecProjectText -notmatch [regex]::Escape($badge)) {
        Add-CecFailure ("project files must declare the {0} configuration" -f $badge)
    }
}

# 6. The former language constraints still hold.
Test-CecPattern -Pattern '\b(namespace|using|throw|try|catch|goto|dynamic_cast|typeid)\b|std::(function|bind|shared_ptr|async)|\.detach\s*\(' -Label 'forbidden C++ construct' -Files $cecSources
Test-CecPattern -Pattern '^\s*#\s*define\b' -Label 'project macro in implementation file' -Files @($cecSources | Where-Object Extension -In @('.c', '.cc', '.cpp', '.cxx'))
Test-CecPattern -Pattern '\b(sendto|recvfrom|WSASend|WSARecv|WSASendTo|WSARecvFrom)\s*\(' -Label 'non-RIO payload API' -Files $cecSources
# The transport API is named send/recv in every language of this workspace, so a bare verb is only
# a raw payload call when the line is neither a qualified member access nor a member declaration.
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
# Only production sources are held to the bare verb rule: a test recorder legitimately has a
# member named send/recv and calls it unqualified from inside its own class.
Test-CecPayloadVerb -Files @($cecSources | Where-Object {
        $_.Extension -in @('.c', '.cc', '.cpp', '.cxx') -and $_.DirectoryName -eq (Join-Path $cecRoot 'src')
    })
Test-CecPattern -Pattern '\b(CreateFile|CreateEvent|CreateMutex|CreateSemaphore|LoadLibrary|GetModuleHandle|MessageBox)\s*\(' -Label 'non-Unicode Windows API' -Files $cecSources
Test-CecPattern -Pattern '^\s*#\s*include\s*[<"][^>"]+\.cpp[>"]' -Label 'implementation file inclusion' -Files $cecSources

# 7. The project stays a Visual Studio project without a CMake build and with no dependency on
#    another implementation of the same contract.
Test-CecPattern -Pattern 'CMakeLists\.txt|add_subdirectory\s*\(|cmake_minimum_required' -Label 'CMake build artefact' -Files @($cecProjects + @(Get-Item -LiteralPath (Join-Path $cecRoot 'README.md') -ErrorAction SilentlyContinue))
# This project is standalone: neither its build declarations nor its scripts may name another
# implementation, and nothing may reach outside the project root.
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
Test-CecPattern -Pattern '\.\.[\\/]|add_subdirectory\s*\(|cmake\s+-S|cmake_minimum_required' -Label 'cross-project dependency' -Files $cecDependencyFiles
if ($cecFailures.Count -ne 0) {
    $cecFailures | ForEach-Object { Write-Error $_ }
    throw "client source policy failed with $($cecFailures.Count) violation(s)"
}

Write-Host 'PASS client named-module source policy: no header source, layered modules, Visual Studio build index'
