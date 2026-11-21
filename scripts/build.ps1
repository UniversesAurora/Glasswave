#Requires -Version 7.2
<#
.SYNOPSIS
    Build Glasswave.sln with MSBuild.

.DESCRIPTION
    Release is the default: foobar2000 shows a blocking "compiled in debug mode"
    dialog for Debug components, which also gets in the way of automated
    screenshots. Release builds still produce a PDB for debugging.

.EXAMPLE
    pwsh -File scripts/build.ps1                          # Release|x64
    pwsh -File scripts/build.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [ValidateSet('x64')]              [string]$Platform = 'x64',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path -LiteralPath (Join-Path $Root 'third_party/foobar2000-sdk/.version'))) {
    throw 'foobar2000 SDK missing - run scripts/bootstrap.ps1 first'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (needs Visual Studio 2022 with the C++ desktop workload)' }

# Every GUID written in the source names one thing: a configuration variable or service whose
# GUID another one also has shares its stored value with it (2026-11-21: the mini player's place
# and the search history overwrote each other). Copy a GUID only by name, never by value.
$guidPattern = '\{\s*0x([0-9a-fA-F]{1,8})\s*,\s*0x([0-9a-fA-F]{1,4})\s*,\s*0x([0-9a-fA-F]{1,4})\s*,\s*\{([^}]*)\}\s*\}'
$guidUses = @{}
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Root 'src') -Recurse -File -Include *.cpp, *.h) {
    $number = 0
    foreach ($line in [System.IO.File]::ReadLines($file.FullName)) {
        $number++
        foreach ($m in [regex]::Matches($line, $guidPattern)) {
            $tail = ([regex]::Matches($m.Groups[4].Value, '0x([0-9a-fA-F]{1,2})') | ForEach-Object {
                    [Convert]::ToByte($_.Groups[1].Value, 16).ToString('X2') }) -join ''
            $key = '{0:X8}-{1:X4}-{2:X4}-{3}' -f [Convert]::ToUInt32($m.Groups[1].Value, 16),
                [Convert]::ToUInt16($m.Groups[2].Value, 16), [Convert]::ToUInt16($m.Groups[3].Value, 16), $tail
            $guidUses[$key] += @("$($file.FullName.Substring($Root.Length + 1)):$number")
        }
    }
}
$duplicates = $guidUses.GetEnumerator() | Where-Object { $_.Value.Count -gt 1 }
if ($duplicates) {
    $duplicates | ForEach-Object { Write-Host "GUID $($_.Key) is written more than once: $($_.Value -join ', ')" }
    throw 'Duplicate GUIDs in the source'
}

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
# /MP starts one cl.exe per logical CPU, each committing up to ~1 GB with the PCH. On a
# machine already near its commit limit that spike reaches the memory watchdog's stop line
# (2026-11-14: 32 compilers took commit from 85% to 99.8% in one second). Keep 25% of the
# limit free and give each compiler 1.2 GB of what is left.
$os = Get-CimInstance Win32_OperatingSystem
$spareBytes = ($os.FreeVirtualMemory - $os.TotalVirtualMemorySize * 0.25) * 1KB
$compilers = [Math]::Clamp([int][Math]::Floor($spareBytes / 1.2GB), 2, [Environment]::ProcessorCount)
& $msbuild (Join-Path $Root 'Glasswave.sln') "-t:$target" "-p:Configuration=$Configuration" "-p:Platform=$Platform" `
    "-p:CL_MPCount=$compilers" -m -nologo -v:minimal '-clp:Summary;ForceNoAlign'
if ($LASTEXITCODE -ne 0) { throw "Build failed (MSBuild exit code $LASTEXITCODE)" }

$dll = Join-Path $Root "build/$Platform/$Configuration/foo_ui_glasswave.dll"
Write-Host "Output: $dll"
