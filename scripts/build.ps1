#Requires -Version 7.2
<#
.SYNOPSIS
    Build Glasswave.sln with MSBuild.

.DESCRIPTION
    Release is the default: foobar2000 shows a blocking "compiled in debug mode"
    dialog for Debug components, which also gets in the way of automated
    screenshots. Release builds still produce a PDB for debugging.

.EXAMPLE
    pwsh -File scripts/build.ps1                          # Release|x64
    pwsh -File scripts/build.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [ValidateSet('x64')]              [string]$Platform = 'x64',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path -LiteralPath (Join-Path $Root 'third_party/foobar2000-sdk/.version'))) {
    throw 'foobar2000 SDK missing - run scripts/bootstrap.ps1 first'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (needs Visual Studio 2022 with the C++ desktop workload)' }

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
& $msbuild (Join-Path $Root 'Glasswave.sln') "-t:$target" "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
    -m -nologo -v:minimal '-clp:Summary;ForceNoAlign'
if ($LASTEXITCODE -ne 0) { throw "Build failed (MSBuild exit code $LASTEXITCODE)" }

$dll = Join-Path $Root "build/$Platform/$Configuration/foo_ui_glasswave.dll"
Write-Host "Output: $dll"
