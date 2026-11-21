#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Source = 'qqmusic_ex.js',
    [string]$Title = '晴天',
    [string]$Artist = '周杰伦',
    [double]$Duration = 269
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $Root '.local/tests/lyrics-build'
if (-not (Test-Path -LiteralPath (Join-Path $output 'quickjs.obj'))) {
    throw 'Run scripts/test-lyrics.ps1 first to build the pinned runtime objects.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$command = @'
call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{1}\src\foo_ui_glasswave" /I"{1}\third_party\quickjs-ng" /I"{1}\third_party\miniz" "{1}\tests\lyrics_source_probe.cpp" "{1}\src\foo_ui_glasswave\lyrics\script_host.cpp" "{1}\src\foo_ui_glasswave\core\text.cpp" quickjs.obj miniz.obj /Felyrics_source_probe.exe /link ole32.lib oleaut32.lib crypt32.lib msxml6.lib ws2_32.lib winhttp.lib
'@ -f $devCommand, $Root
Push-Location $output
try {
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'Building lyric-source probe failed' }
    & ./lyrics_source_probe.exe $Root $Source $Title $Artist $Duration.ToString([Globalization.CultureInfo]::InvariantCulture)
    if ($LASTEXITCODE -notin 0,2) { throw "Lyric-source probe failed (exit code $LASTEXITCODE)" }
} finally { Pop-Location }
