#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run standalone 3-track LRC parsing and projection unit checks.
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
$testSource = Join-Path $Root 'tests/lyrics_projection_tests.cpp'
$lrcSource = Join-Path $Root 'src/foo_ui_glasswave/lyrics/lrc.cpp'
$output = Join-Path $Root '.local/tests/lyrics-projection'
$stubDir = Join-Path $output 'stubs'
New-Item -ItemType Directory -Force -Path $output, $stubDir | Out-Null

$pchStub = @'
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cwctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>
'@
Set-Content -LiteralPath (Join-Path $stubDir 'pch.h') -Value $pchStub -Encoding ASCII

$testObj = Join-Path $output 'lyrics_projection_tests.obj'
$lrcObj = Join-Path $output 'lrc_standalone.obj'
$executable = Join-Path $output 'lyrics_projection_tests.exe'
$command = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /FI"{1}" /I"{2}" /I"{3}" /c "{4}" /Fo"{5}" && ' +
    'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /I"{2}" /I"{3}" /c "{6}" /Fo"{7}" && ' +
    'link /nologo /OUT:"{8}" "{5}" "{7}"'
) -f $devCommand, (Join-Path $stubDir 'pch.h'), $stubDir, $include, $testSource, $testObj, $lrcSource, $lrcObj, $executable

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Compiling lyrics projection checks failed (exit code $LASTEXITCODE)" }
& $executable
if ($LASTEXITCODE -ne 0) { throw "Lyrics projection checks failed (exit code $LASTEXITCODE)" }
