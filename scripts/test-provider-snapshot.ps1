#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run the standalone provider snapshot regression checks.
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
$testSource = Join-Path $Root 'tests/provider_snapshot_tests.cpp'
$storeSource = Join-Path $Root 'src/foo_ui_glasswave/cache/store.cpp'
$snapshotSource = Join-Path $Root 'src/foo_ui_glasswave/cache/provider_snapshot.cpp'
$textSource = Join-Path $Root 'src/foo_ui_glasswave/core/text.cpp'
$output = Join-Path $Root '.local/tests'
$testObject = Join-Path $output 'provider_snapshot_tests.obj'
$storeObject = Join-Path $output 'provider_snapshot_store.obj'
$snapshotObject = Join-Path $output 'provider_snapshot.obj'
$textObject = Join-Path $output 'provider_snapshot_text.obj'
$executable = Join-Path $output 'provider_snapshot_tests.exe'
$testRoot = $null

New-Item -ItemType Directory -Force -Path $output | Out-Null

function Remove-SafeTestRoot {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$Base
    )

    $fullBase = [IO.Path]::GetFullPath($Base).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $prefix = $fullBase + [IO.Path]::DirectorySeparatorChar
    $name = [IO.Path]::GetFileName($fullPath)
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not $name.StartsWith('provider-snapshot-', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an unsafe provider snapshot test root: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

$compilePrefix = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 ' +
    '/DWIN32_LEAN_AND_MEAN /DNOMINMAX /DGLASSWAVE_CACHE_STANDALONE /I"{1}"'
) -f $devCommand, $include

try {
    $testBase = Join-Path $Root '.local/tests'
    $testRoot = Join-Path $testBase ('provider-snapshot-' + [Guid]::NewGuid().ToString('N'))
    $command = (
        $compilePrefix +
        ' /c "{0}" /Fo"{1}" && ' +
        'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN ' +
        '/DNOMINMAX /DGLASSWAVE_CACHE_STANDALONE /I"{2}" /c "{3}" /Fo"{4}" && ' +
        'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN ' +
        '/DNOMINMAX /DGLASSWAVE_CACHE_STANDALONE /I"{2}" /c "{6}" /Fo"{7}" && ' +
        'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN ' +
        '/DNOMINMAX /I"{2}" /c "{8}" /Fo"{9}" && ' +
        'cl /nologo "{1}" "{4}" "{7}" "{9}" /Fe"{5}" /link bcrypt.lib winsqlite3.lib advapi32.lib ole32.lib'
    ) -f $testSource, $testObject, $include, $storeSource, $storeObject, $executable, $snapshotSource,
        $snapshotObject, $textSource, $textObject
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Compiling provider snapshot checks failed (exit code $LASTEXITCODE)"
    }

    Push-Location -LiteralPath $Root
    try {
        & $executable $testRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Provider snapshot checks failed (exit code $LASTEXITCODE)"
        }
    } finally {
        Pop-Location
    }
} finally {
    if ($testRoot) {
        Remove-SafeTestRoot -Path $testRoot -Base (Join-Path $Root '.local/tests')
    }
}
