param(
    [Parameter(Mandatory)]
    [string] $ProjectRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$cecRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$cecSources = @(Get-ChildItem -LiteralPath (Join-Path $cecRoot 'include'), (Join-Path $cecRoot 'src'),
    (Join-Path $cecRoot 'tests') -File -Recurse | Where-Object {
        $_.Extension -in @('.h', '.hpp', '.c', '.cc', '.cpp', '.cxx')
    })
$cecFailures = [System.Collections.Generic.List[string]]::new()

function Test-CecPattern {
    param([string] $Pattern, [string] $Label, [System.IO.FileInfo[]] $Files)
    foreach ($file in $Files) {
        $matches = @(Select-String -LiteralPath $file.FullName -Pattern $Pattern -AllMatches)
        foreach ($match in $matches) {
            $cecFailures.Add("$Label`: $($file.FullName):$($match.LineNumber)")
        }
    }
}

Test-CecPattern -Pattern '\b(namespace|using|throw|try|catch|goto|dynamic_cast|typeid)\b|std::(function|bind|shared_ptr|async)|\.detach\s*\(' -Label 'forbidden C++ construct' -Files $cecSources
Test-CecPattern -Pattern '^\s*#\s*define\b' -Label 'project macro in implementation file' -Files @($cecSources | Where-Object Extension -In @('.c', '.cc', '.cpp', '.cxx'))
Test-CecPattern -Pattern '^\s*struct\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:\{|;)' -Label 'plain structure declared in implementation file' -Files @($cecSources | Where-Object Extension -In @('.c', '.cc', '.cpp', '.cxx'))
Test-CecPattern -Pattern '\b(send|recv|sendto|recvfrom|WSASend|WSARecv)\s*\(' -Label 'non-RIO payload API' -Files $cecSources
Test-CecPattern -Pattern '\b(CreateFile|CreateEvent|CreateMutex|CreateSemaphore|LoadLibrary|GetModuleHandle|MessageBox)\s*\(' -Label 'non-Unicode Windows API' -Files $cecSources

$cecOwnedFiles = @(Get-ChildItem -LiteralPath $cecRoot -File -Recurse | Where-Object {
        $_.FullName -notmatch '[\\/]build[\\/]' -and $_.FullName -notmatch '[\\/]tools[\\/]verification[\\/]' -and $_.FullName -ne $PSCommandPath
    })
Test-CecPattern -Pattern 'cpp-echo-server|\.\.[\\/].*(common|shared)|add_subdirectory\s*\(' -Label 'cross-project dependency' -Files $cecOwnedFiles

if ($cecFailures.Count -ne 0) {
    $cecFailures | ForEach-Object { Write-Error $_ }
    throw "client source policy failed with $($cecFailures.Count) violation(s)"
}

Write-Host 'PASS client source policy'

