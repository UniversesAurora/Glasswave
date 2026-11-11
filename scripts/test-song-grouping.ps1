#Requires -Version 7.2
<#
.SYNOPSIS
    Check remote expanded-tile album grouping without building the component.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vcTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$visualStudio = & $vswhere -latest -requires $vcTools -property installationPath |
    Select-Object -First 1
if (-not $visualStudio) { throw 'Visual Studio C++ tools not found' }
$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$include = Join-Path $Root 'src/foo_ui_glasswave'
$source = Join-Path $Root 'tests/song_grouping_tests.cpp'
$output = Join-Path $Root '.local/tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$object = Join-Path $output 'song_grouping_tests.obj'
$executable = Join-Path $output 'song_grouping_tests.exe'
$command = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /I"{1}" "{2}" ' +
    '/Fo"{3}" /Fe"{4}"'
) -f $devCommand, $include, $source, $object, $executable
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Compiling song grouping checks failed (exit code $LASTEXITCODE)" }
& $executable
if ($LASTEXITCODE -ne 0) { throw "Song grouping checks failed (exit code $LASTEXITCODE)" }
