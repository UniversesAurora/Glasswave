#Requires -Version 7.2
<#
.SYNOPSIS
    Load and unload the built component in an isolated child process without foobar2000.

.DESCRIPTION
    The child resolves shared.dll and the Microsoft runtime only from the repository
    dev root. A DLL_PROCESS_ATTACH crash remains isolated from this PowerShell process.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$component = Join-Path $Root "build/x64/$Configuration/foo_ui_glasswave.dll"
$runtimeRoot = Join-Path $Root '.local/fb2k-dev'
if (-not (Test-Path -LiteralPath $component -PathType Leaf)) {
    throw "Built component missing for $Configuration x64"
}
foreach ($name in 'shared.dll', 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot $name) -PathType Leaf)) {
        throw "Runtime dependency missing from the repository dev root: $name"
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vcTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$visualStudio = & $vswhere -latest -requires $vcTools -property installationPath |
    Select-Object -First 1
if (-not $visualStudio) { throw 'Visual Studio C++ tools not found' }

$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$source = Join-Path $Root 'tests/test_component_load.cpp'
$output = Join-Path $Root '.local/tests/component-load'
$object = Join-Path $output 'test_component_load.obj'
$executable = Join-Path $output 'test_component_load.exe'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$command = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DNDEBUG ' +
    '/DWIN32_LEAN_AND_MEAN /DNOMINMAX "{1}" /Fo"{2}" /Fe"{3}"'
) -f $devCommand, $source, $object, $executable

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Compiling component-load checks failed (exit code $LASTEXITCODE)"
}

Push-Location -LiteralPath $output
try {
    & $executable $component $runtimeRoot
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        if ($exitCode -lt 0) {
            throw "Component load child crashed (exit code 0x$('{0:X8}' -f ([uint32]$exitCode)))"
        }
        throw "Component load child failed (exit code $exitCode)"
    }
} finally {
    Pop-Location
}
