#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run standalone Apple Music regional DisplayMetadata persistence and fallback checks.
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
$testSource = Join-Path $Root 'tests/apple_display_metadata_tests.cpp'
$dmSource = Join-Path $Root 'src/foo_ui_glasswave/applemusic/display_metadata.cpp'
$textSource = Join-Path $Root 'src/foo_ui_glasswave/core/text.cpp'
$tsvSource = Join-Path $Root 'src/foo_ui_glasswave/core/tsv.cpp'
$output = Join-Path $Root '.local/tests'
$testRoot = Join-Path $output ('apple-display-metadata-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $output, $testRoot | Out-Null

try {
    $stubDir = Join-Path $testRoot 'stubs'
    New-Item -ItemType Directory -Force -Path $stubDir | Out-Null
    Set-Content -LiteralPath (Join-Path $stubDir 'pch.h') -Value '#pragma once' -Encoding ASCII
    $testObj = Join-Path $output 'apple_display_metadata_tests.obj'
    $dmObj = Join-Path $output 'display_metadata_standalone.obj'
    $textObj = Join-Path $output 'core_text_standalone.obj'
    $tsvObj = Join-Path $output 'core_tsv_standalone.obj'
    $executable = Join-Path $output 'apple_display_metadata_tests.exe'
    $command = (
        'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
        'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /DGLASSWAVE_DISPLAY_METADATA_STANDALONE /I"{11}" /I"{1}" /c "{2}" /Fo"{3}" && ' +
        'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /DGLASSWAVE_DISPLAY_METADATA_STANDALONE /I"{11}" /I"{1}" /c "{4}" /Fo"{5}" && ' +
        'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /I"{1}" /c "{6}" /Fo"{7}" && ' +
        'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /I"{11}" /I"{1}" /c "{8}" /Fo"{9}" && ' +
        'link /nologo /OUT:"{10}" "{3}" "{5}" "{7}" "{9}"'
    ) -f $devCommand, $include, $testSource, $testObj, $dmSource, $dmObj, $textSource, $textObj, $tsvSource, $tsvObj, $executable, $stubDir
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Compiling Apple DisplayMetadata checks failed (exit code $LASTEXITCODE)" }
    & $executable $testRoot
    if ($LASTEXITCODE -ne 0) { throw "Apple DisplayMetadata checks failed (exit code $LASTEXITCODE)" }
} finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
