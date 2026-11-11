#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run the standalone persisted list-cache regression checks.

.PARAMETER CheckOnly
    Compile the list-cache test translation unit without linking or running it.

.PARAMETER Repeat
    Run the complete list-cache regression this many times.
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,

    [ValidateRange(1, 20)]
    [int]$Repeat = 1
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
$testSource = Join-Path $Root 'tests/list_cache_tests.cpp'
$storeSource = Join-Path $Root 'src/foo_ui_glasswave/cache/store.cpp'
$output = Join-Path $Root '.local/tests'
$testObject = Join-Path $output 'list_cache_tests.obj'
$storeObject = Join-Path $output 'list_cache_store.obj'
$executable = Join-Path $output 'list_cache_tests.exe'

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
        -not $name.StartsWith('list-cache-', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an unsafe list-cache test root: $fullPath"
    }
    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

$commonOptions = (
    '/nologo /std:c++20 /EHsc /W4 /WX /utf-8 /permissive- /O2 /DNDEBUG ' +
    '/DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /I"{0}"'
) -f $include
$compilePrefix = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && cl {1}' -f `
    $devCommand, $commonOptions

if ($CheckOnly) {
    $checkObject = Join-Path $output 'list_cache_tests.check.obj'
    $command = $compilePrefix + ' /c "{0}" /Fo"{1}"' -f $testSource, $checkObject
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Compiling list-cache checks failed (exit code $LASTEXITCODE)"
    }
    return
}

$command = (
    $compilePrefix + ' /c "{0}" /Fo"{1}" && ' +
    'cl {2} /DGLASSWAVE_CACHE_STANDALONE /c "{3}" /Fo"{4}" && ' +
    'cl /nologo "{1}" "{4}" /Fe"{5}" /link ' +
    'bcrypt.lib winsqlite3.lib advapi32.lib ole32.lib runtimeobject.lib windowsapp.lib'
) -f $testSource, $testObject, $commonOptions, $storeSource, $storeObject, $executable
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Compiling list-cache checks failed (exit code $LASTEXITCODE)"
}

for ($iteration = 1; $iteration -le $Repeat; ++$iteration) {
    $testRoot = Join-Path $output ('list-cache-' + [Guid]::NewGuid().ToString('N'))
    try {
        Push-Location -LiteralPath $Root
        try {
            & $executable $testRoot
            if ($LASTEXITCODE -ne 0) {
                throw "List-cache checks failed (iteration $iteration, exit code $LASTEXITCODE)"
            }
        } finally {
            Pop-Location
        }
    } finally {
        Remove-SafeTestRoot -Path $testRoot -Base $output
    }
}
