#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run the standalone shared-cache store regression checks.
.PARAMETER CheckOnly
    Compile the test source against cache/store.h without linking or running it.
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly
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
$testSource = Join-Path $Root 'tests/cache_store_tests.cpp'
$storeSource = Join-Path $Root 'src/foo_ui_glasswave/cache/store.cpp'
$output = Join-Path $Root '.local/tests'
$testObject = Join-Path $output 'cache_store_tests.obj'
$storeObject = Join-Path $output 'cache_store.obj'
$executable = Join-Path $output 'cache_store_tests.exe'
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
        -not $name.StartsWith('cache-store-', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an unsafe cache test root: $fullPath"
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
    if ($CheckOnly) {
        $checkObject = Join-Path $output 'cache_store_tests.check.obj'
        $command = $compilePrefix + ' /c "{0}" /Fo"{1}"' -f $testSource, $checkObject
        & $env:ComSpec /d /s /c $command
        if ($LASTEXITCODE -ne 0) {
            throw "Compiling cache store checks failed (exit code $LASTEXITCODE)"
        }
        return
    }

    if (-not (Test-Path -LiteralPath $storeSource -PathType Leaf)) {
        throw "Cache store implementation is not ready: $storeSource"
    }

    $testBase = Join-Path $Root '.local/tests'
    $testRoot = Join-Path $testBase ('cache-store-' + [Guid]::NewGuid().ToString('N'))
    $command = (
        $compilePrefix +
        ' /c "{0}" /Fo"{1}" && ' +
        'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN ' +
        '/DNOMINMAX /DGLASSWAVE_CACHE_STANDALONE /I"{2}" /c "{3}" /Fo"{4}" && ' +
        'cl /nologo "{1}" "{4}" /Fe"{5}" /link bcrypt.lib winsqlite3.lib advapi32.lib ole32.lib'
    ) -f $testSource, $testObject, $include, $storeSource, $storeObject, $executable
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Compiling cache store checks failed (exit code $LASTEXITCODE)"
    }

    Push-Location -LiteralPath $Root
    try {
        & $executable $testRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Cache store checks failed (exit code $LASTEXITCODE)"
        }
    } finally {
        Pop-Location
    }
} finally {
    if ($testRoot) {
        Remove-SafeTestRoot -Path $testRoot -Base (Join-Path $Root '.local/tests')
    }
}
