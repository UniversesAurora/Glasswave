#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run standalone lyrics request-state contract checks.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $Root 'third_party/foobar2000-sdk'
if (-not (Test-Path -LiteralPath (Join-Path $sdk '.version') -PathType Leaf)) {
    throw 'foobar2000 SDK missing - run scripts/bootstrap.ps1 first'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vcTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$visualStudio = & $vswhere -latest -requires $vcTools -property installationPath |
    Select-Object -First 1
if (-not $visualStudio) { throw 'Visual Studio C++ tools not found' }

$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$sourceInclude = Join-Path $Root 'src/foo_ui_glasswave'
$sdkInclude = Join-Path $sdk 'foobar2000'
$source = Join-Path $Root 'tests/lyrics_request_state_tests.cpp'
$sharedLibrary = Join-Path $sdk 'foobar2000/shared/shared-x64.lib'
$output = Join-Path $Root '.local/tests/lyrics-request-state'
$object = Join-Path $output 'lyrics_request_state_tests.obj'
$executable = Join-Path $output 'lyrics_request_state_tests.exe'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$command = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /Y- /DNDEBUG /wd4125 ' +
    '/DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{1}" /I"{2}" /I"{3}" ' +
    '"{4}" /Fo"{5}" /Fe"{6}" /link "{7}"'
) -f $devCommand, $sourceInclude, $sdkInclude, $sdk, $source, $object, $executable,
    $sharedLibrary

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Compiling lyrics request-state checks failed (exit code $LASTEXITCODE)"
}

Push-Location -LiteralPath $output
try {
    & $executable
    if ($LASTEXITCODE -ne 0) {
        throw "Lyrics request-state checks failed (exit code $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}
