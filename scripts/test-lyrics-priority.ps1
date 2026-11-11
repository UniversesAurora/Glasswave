#Requires -Version 7.2
<#
.SYNOPSIS
    Compile and run dependency-light unified lyrics ordering, cache and manual-selection checks.
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
$source = Join-Path $Root 'tests/lyrics_priority_tests.cpp'
$output = Join-Path $Root '.local/tests/lyrics-priority'
$object = Join-Path $output 'lyrics_priority_tests.obj'
$executable = Join-Path $output 'lyrics_priority_tests.exe'
New-Item -ItemType Directory -Force -Path $output | Out-Null

$command = (
    'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && ' +
    'cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /DUNICODE /D_UNICODE /I"{1}" ' +
    '/c "{2}" /Fo"{3}" && cl /nologo "{3}" /Fe"{4}"'
) -f $devCommand, $include, $source, $object, $executable

& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Compiling lyrics priority checks failed (exit code $LASTEXITCODE)"
}

& $executable
if ($LASTEXITCODE -ne 0) {
    throw "Lyrics priority checks failed (exit code $LASTEXITCODE)"
}

$translations = Get-Content -LiteralPath (Join-Path $Root 'lang/zh-CN.json') -Raw |
    ConvertFrom-Json -AsHashtable
$sourceNames = @(
    'File embedded (Source)',
    'Same-name lyric file (Source)',
    'NetEase Cloud Music (Source)',
    'Apple Music (Source)'
)
foreach ($name in $sourceNames) {
    if (-not $translations.ContainsKey($name) -or
        [string]$translations[$name] -notmatch '（来源）$') {
        throw "Missing localized native lyric source name: $name"
    }
}
