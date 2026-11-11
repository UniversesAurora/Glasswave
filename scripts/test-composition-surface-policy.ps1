#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run the non-GUI retained-surface policy checks.
.PARAMETER CheckOnly
    Compile without linking or running.
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vcTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$visualStudio = & $vswhere -latest -requires $vcTools -property installationPath |
    Select-Object -First 1
if (-not $visualStudio) { throw 'Visual Studio C++ tools not found' }

$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$include = Join-Path $Root 'src/foo_ui_glasswave'
$source = Join-Path $Root 'tests/composition_surface_policy_tests.cpp'
$output = Join-Path $Root '.local/tests'
$object = Join-Path $output 'composition_surface_policy_tests.obj'
$executable = Join-Path $output 'composition_surface_policy_tests.exe'

New-Item -ItemType Directory -Force -Path $output | Out-Null

function Invoke-VsDevCommand {
    param([Parameter(Mandatory)][string] $Command)

    $commandLine = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && {1}' -f $devCommand, $Command
    & $env:ComSpec /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio command failed (exit code $LASTEXITCODE)"
    }
}

$compile = (
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ' +
    '/I"{0}" /c "{1}" /Fo"{2}"'
) -f $include, $source, $object
Invoke-VsDevCommand $compile
if ($CheckOnly) { return }

$link = 'cl /nologo "{0}" /Fe"{1}" /link windowsapp.lib runtimeobject.lib' -f $object, $executable
Invoke-VsDevCommand $link

Push-Location -LiteralPath $Root
try {
    & $executable
    if ($LASTEXITCODE -ne 0) {
        throw "Composition surface policy checks failed (exit code $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}
