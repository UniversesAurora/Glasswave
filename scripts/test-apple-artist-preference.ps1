#Requires -Version 7.2
<#
.SYNOPSIS
    Check Apple favorite-artist state, identity, and request construction.
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
$sdkRoot = Join-Path $Root 'third_party/foobar2000-sdk'
$quickJs = Join-Path $Root 'third_party/quickjs-ng'
$miniz = Join-Path $Root 'third_party/miniz'
$output = Join-Path $Root '.local/tests/apple-artist-preference'
$helper = Join-Path $Root 'src/foo_ui_glasswave/applemusic/artist_preference.cpp'
$source = Join-Path $Root 'tests/apple_artist_preference_tests.cpp'
$helperObject = Join-Path $output 'artist_preference.obj'
$testObject = Join-Path $output 'apple_artist_preference_tests.obj'
$executable = Join-Path $output 'apple_artist_preference_tests.exe'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$compile = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /DUNICODE /D_UNICODE ' +
    '/DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WINDOWS /DNDEBUG /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ' +
    '/I"{1}" /I"{2}" /I"{3}" /I"{4}" /I"{5}" /c "{6}" /Fo"{7}" && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /DUNICODE /D_UNICODE ' +
    '/I"{1}" /c "{8}" /Fo"{9}" && ' +
    'cl /nologo /EHsc "{7}" "{9}" /Fe"{10}" /link windowsapp.lib'
) -f $devCommand, $include, $quickJs, $miniz, (Join-Path $sdkRoot 'foobar2000'), $sdkRoot,
    $helper, $helperObject, $source, $testObject, $executable

& $env:ComSpec /d /s /c $compile
if ($LASTEXITCODE -ne 0) {
    throw "Compiling Apple artist preference checks failed (exit code $LASTEXITCODE)"
}

& $executable
if ($LASTEXITCODE -ne 0) {
    throw "Apple artist preference checks failed (exit code $LASTEXITCODE)"
}
