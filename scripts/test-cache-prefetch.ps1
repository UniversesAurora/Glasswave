#Requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
if (-not $vs) { throw 'Visual Studio C++ tools not found.' }
$output = Join-Path $root '.local/tests/cache-prefetch'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$command = ('call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /I"{1}" "{2}" "{3}" /Fo"{4}\\" /Fe"{5}"') -f
    (Join-Path $vs 'Common7/Tools/VsDevCmd.bat'),
    (Join-Path $root 'src/foo_ui_glasswave'),
    (Join-Path $root 'tests/cache_prefetch_tests.cpp'),
    (Join-Path $root 'src/foo_ui_glasswave/cache/prefetch.cpp'),
    $output, (Join-Path $output 'cache_prefetch_tests.exe')
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw 'Prefetch coordinator compilation failed.' }
& (Join-Path $output 'cache_prefetch_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Prefetch coordinator regression failed.' }
