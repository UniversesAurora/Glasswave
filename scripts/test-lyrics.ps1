#Requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
if (-not $visualStudio) { throw 'Visual Studio C++ tools not found' }
$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$output = Join-Path $Root '.local/tests/lyrics-build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$include = Join-Path $Root 'src/foo_ui_glasswave'
$quickjs = Join-Path $Root 'third_party/quickjs-ng'
$miniz = Join-Path $Root 'third_party/miniz'
$command = @'
call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && cl /nologo /c /O2 /w /std:c11 /experimental:c11atomics /D_CRT_SECURE_NO_WARNINGS "{1}\quickjs-amalgam.c" /Foquickjs.obj && cl /nologo /c /O2 /w /D_CRT_SECURE_NO_WARNINGS "{2}\miniz.c" /Fominiz.obj && cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{3}" /I"{1}" /I"{2}" "{4}\tests\lyrics_script_tests.cpp" "{3}\lyrics\script_host.cpp" "{3}\core\text.cpp" quickjs.obj miniz.obj /Felyrics_script_tests.exe /link ole32.lib oleaut32.lib crypt32.lib msxml6.lib ws2_32.lib
'@ -f $devCommand, $quickjs, $miniz, $include, $Root
Push-Location $output
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Compiling lyric checks failed (exit code $LASTEXITCODE)" }
    & ./lyrics_script_tests.exe $Root
    if ($LASTEXITCODE -ne 0) { throw "Lyric checks failed (exit code $LASTEXITCODE)" }
    $httpCommand = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{1}" "{2}\tests\http_request_tests.cpp" /Fehttp_request_tests.exe /link winhttp.lib ws2_32.lib' -f
        $devCommand, $include, $Root
    & $env:ComSpec /d /s /c $httpCommand
    if ($LASTEXITCODE -ne 0) { throw "Compiling HTTP checks failed (exit code $LASTEXITCODE)" }
    & ./http_request_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "HTTP checks failed (exit code $LASTEXITCODE)" }
} finally { Pop-Location }
