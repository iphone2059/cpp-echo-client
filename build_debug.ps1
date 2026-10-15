# Debug wrapper: forwards to the Visual Studio build and verification entry point.
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Debug @args
exit $LASTEXITCODE
