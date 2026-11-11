#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run standalone FrameClock failure-pacing regression checks.
.PARAMETER CheckOnly
    Compile the test and implementation without linking or running it.
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [ValidateSet('all', 'compositor-wake', 'healthy', 'immediate-failure', 'mixed-failure', 'stop-idle')]
    [string]$Case = 'all'
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
$testSource = Join-Path $Root 'tests/frame_clock_tests.cpp'
$clockSource = Join-Path $Root 'src/foo_ui_glasswave/gfx/frame_clock.cpp'
$output = Join-Path $Root '.local/tests'
$standaloneInclude = Join-Path $output 'standalone-include'
$standalonePch = Join-Path $standaloneInclude 'pch.h'
$testObject = Join-Path $output 'frame_clock_tests.obj'
$clockObject = Join-Path $output 'frame_clock.obj'
$productionObject = Join-Path $output 'frame_clock.production.obj'
$executable = Join-Path $output 'frame_clock_tests.exe'

New-Item -ItemType Directory -Force -Path $output | Out-Null
New-Item -ItemType Directory -Force -Path $standaloneInclude | Out-Null
@'
#pragma once

#include <Windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <utility>
'@ | Set-Content -LiteralPath $standalonePch -Encoding utf8

$commonCl = (
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN ' +
    '/DNOMINMAX /DGLASSWAVE_FRAME_CLOCK_STANDALONE /I"{0}" /I"{1}"'
) -f $standaloneInclude, $include
$testCl = $commonCl + ' /DGLASSWAVE_FRAME_CLOCK_TEST'

function Invoke-VsDevCommand {
    param([Parameter(Mandatory)][string] $Command)

    $commandLine = (
        'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && {1}'
    ) -f $devCommand, $Command
    & $env:ComSpec /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio command failed (exit code $LASTEXITCODE)"
    }
}

$compileTest = '{0} /c "{1}" /Fo"{2}"' -f $testCl, $testSource, $testObject
$compileClockTest = '{0} /c "{1}" /Fo"{2}"' -f $testCl, $clockSource, $clockObject
$compileClockProduction = '{0} /c "{1}" /Fo"{2}"' -f $commonCl, $clockSource, $productionObject

if ($CheckOnly) {
    $testCheck = Join-Path $output 'frame_clock_tests.check.obj'
    $clockCheck = Join-Path $output 'frame_clock.check.obj'
    $compileClockCheck = '{0} /c "{1}" /Fo"{2}"' -f $testCl, $clockSource, $clockCheck
    Invoke-VsDevCommand $compileTest
    Invoke-VsDevCommand $compileClockCheck
    Invoke-VsDevCommand $compileClockProduction
    return
} else {
    Invoke-VsDevCommand $compileTest
    Invoke-VsDevCommand $compileClockTest
    Invoke-VsDevCommand $compileClockProduction
}

$link = 'cl /nologo "{0}" "{1}" /Fe"{2}" /link dwmapi.lib user32.lib' -f $testObject,
    $clockObject, $executable
Invoke-VsDevCommand $link

Push-Location -LiteralPath $Root
try {
        & $executable $Case
    if ($LASTEXITCODE -ne 0) {
        throw "FrameClock checks failed (exit code $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}
