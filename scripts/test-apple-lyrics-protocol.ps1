#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run the dependency-light Apple lyrics bridge protocol checks.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
if (-not $visualStudio) { throw 'Visual Studio C++ tools not found' }

$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$include = Join-Path $Root 'src/foo_ui_glasswave'
$sdkRoot = Join-Path $Root 'third_party/foobar2000-sdk'
$quickJs = Join-Path $Root 'third_party/quickjs-ng'
$miniz = Join-Path $Root 'third_party/miniz'
$output = Join-Path $Root '.local/tests/apple-lyrics-protocol'
$protocol = Join-Path $include 'applemusic/lyrics_protocol.cpp'
$json = Join-Path $include 'netease/json.cpp'
$source = Join-Path $Root 'tests/apple_lyrics_protocol_tests.cpp'
$protocolObject = Join-Path $output 'applemusic_lyrics_protocol.obj'
$jsonObject = Join-Path $output 'netease_json.obj'
$testObject = Join-Path $output 'apple_lyrics_protocol_tests.obj'
$executable = Join-Path $output 'apple_lyrics_protocol_tests.exe'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$common = (
    '/nologo /std:c++20 /EHsc /W4 /WX /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ' +
    '/DNOMINMAX /D_WINDOWS /DNDEBUG /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ' +
    '/I"{0}" /I"{1}" /I"{2}" /I"{3}" /I"{4}"'
) -f $include, (Join-Path $sdkRoot 'foobar2000'), $sdkRoot, $quickJs, $miniz
$command = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl {1} /c "{2}" /Fo"{3}" && cl {1} /c "{4}" /Fo"{5}" && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /I"{6}" /c "{7}" /Fo"{8}" && ' +
    'cl /nologo /EHsc "{3}" "{5}" "{8}" /Fe"{9}" /link windowsapp.lib'
) -f $devCommand, $common, $protocol, $protocolObject, $json, $jsonObject, $include, $source,
    $testObject, $executable

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Compiling Apple lyrics protocol checks failed (exit code $LASTEXITCODE)"
}

& $executable
if ($LASTEXITCODE -ne 0) {
    throw "Apple lyrics protocol checks failed (exit code $LASTEXITCODE)"
}
