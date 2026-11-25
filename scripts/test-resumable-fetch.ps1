#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run standalone resumable fetch stall-timeout and Range-resume tests.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $Root 'third_party/foobar2000-sdk'
if (-not (Test-Path -LiteralPath (Join-Path $sdk '.version') -PathType Leaf)) {
    throw 'foobar2000 SDK missing - run scripts/bootstrap.ps1 first'
}

$pfcLib = Join-Path $Root 'x64/Release/pfc.lib'
$sdkLib = Join-Path $Root 'x64/Release/foobar2000_SDK.lib'
$sharedLib = Join-Path $sdk 'foobar2000/shared/shared-x64.lib'
foreach ($lib in @($pfcLib, $sdkLib, $sharedLib)) {
    if (-not (Test-Path -LiteralPath $lib -PathType Leaf)) {
        throw "Required library missing ($lib) - run scripts/build.ps1 first"
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vcTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$visualStudio = & $vswhere -latest -requires $vcTools -property installationPath |
    Select-Object -First 1
if (-not $visualStudio) { throw 'Visual Studio C++ tools not found' }

$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$sourceInclude = Join-Path $Root 'src/foo_ui_glasswave'
$sdkInclude = Join-Path $sdk 'foobar2000'
$testSource = Join-Path $Root 'tests/resumable_fetch_tests.cpp'
$fetchSource = Join-Path $Root 'src/foo_ui_glasswave/net/resumable_fetch.cpp'
$output = Join-Path $Root '.local/tests/resumable-fetch'
$testObject = Join-Path $output 'resumable_fetch_tests.obj'
$fetchObject = Join-Path $output 'resumable_fetch.obj'
$executable = Join-Path $output 'resumable_fetch_tests.exe'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$sharedDll = Join-Path $Root '.local/fb2k-dev/shared.dll'
if (Test-Path -LiteralPath $sharedDll -PathType Leaf) {
    Copy-Item -LiteralPath $sharedDll -Destination (Join-Path $output 'shared.dll') -Force
}

$flags = (
    '/nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /MD /Y- /DNDEBUG /wd4125 ' +
    '/DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{0}" /I"{1}" /I"{2}"'
) -f $sourceInclude, $sdkInclude, $sdk

$command = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl {1} /c "{2}" /Fo"{3}" && ' +
    'cl {1} /c "{4}" /Fo"{5}" && ' +
    'cl /nologo "{3}" "{5}" /Fe"{6}" /link /LTCG "{7}" "{8}" "{9}" kernel32.lib user32.lib ole32.lib shell32.lib shlwapi.lib'
) -f $devCommand, $flags, $testSource, $testObject, $fetchSource, $fetchObject, $executable,
    $sdkLib, $pfcLib, $sharedLib

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Compiling resumable fetch tests failed (exit code $LASTEXITCODE)"
}

Push-Location -LiteralPath $output
try {
    & $executable
    if ($LASTEXITCODE -ne 0) {
        throw "Resumable fetch tests failed (exit code $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}
