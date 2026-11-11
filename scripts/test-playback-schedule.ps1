#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run deterministic Glasswave playback-schedule checks.
#>
[CmdletBinding()]
param(
    [switch]$CompileOnly
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$VisualStudio = & $VsWhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1
if (-not $VisualStudio) { throw 'Visual Studio C++ tools not found' }

$DevCommand = Join-Path $VisualStudio 'Common7/Tools/VsDevCmd.bat'
$Include = Join-Path $Root 'src/foo_ui_glasswave'
$Source = Join-Path $Root 'tests/playback_schedule_tests.cpp'
$ScheduleSource = Join-Path $Root 'src/foo_ui_glasswave/media/playback_schedule.cpp'
$OutputDirectory = Join-Path $Root '.local/tests'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$TestObject = Join-Path $OutputDirectory 'playback_schedule_tests.obj'
$ScheduleObject = Join-Path $OutputDirectory 'playback_schedule.obj'
$Executable = Join-Path $OutputDirectory 'playback_schedule_tests.exe'

$Compile = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /I"{1}" /c "{2}" /Fo"{3}" && ' +
    'cl /nologo /std:c++20 /O2 /EHsc /W4 /WX /I"{1}" /c "{4}" /Fo"{5}" && ' +
    'link /nologo /out:"{6}" "{3}" "{5}"'
) -f $DevCommand, $Include, $Source, $TestObject, $ScheduleSource, $ScheduleObject, $Executable
& $env:ComSpec /d /s /c $Compile
if ($LASTEXITCODE -ne 0) { throw "Compiling playback-schedule checks failed (exit code $LASTEXITCODE)" }

if (-not $CompileOnly) {
    & $Executable
    if ($LASTEXITCODE -ne 0) { throw "Playback-schedule checks failed (exit code $LASTEXITCODE)" }
}
