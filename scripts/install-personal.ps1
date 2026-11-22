#Requires -Version 7.2
<#
.SYNOPSIS
    Install or update Glasswave and owner-local non-redistributable assets in the
    owner's production foobar2000 installation.

.DESCRIPTION
    Only updates component binaries, translations, built-in scripts, and local assets
    (Win7 artwork, Apple Android .so libraries, local lyric scripts). Never touches
    existing accounts, playlists, media library, config.sqlite, or caches.

    Refuses to run while the target foobar2000.exe is active.

.EXAMPLE
    pwsh -File scripts/install-personal.ps1
    pwsh -File scripts/install-personal.ps1 -Build
    pwsh -File scripts/install-personal.ps1 -InstallDir 'E:\music\foobar2000' -ProfileDir "$env:APPDATA\foobar2000-v2"
#>
[CmdletBinding()]
param(
    [string]$InstallDir = 'E:\music\foobar2000',

    [string]$ProfileDir = (Join-Path $env:APPDATA 'foobar2000-v2'),

    [string]$PackagePath,

    [switch]$Build,

    [switch]$SkipLocalAssets
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$ProfileDir = [IO.Path]::GetFullPath($ProfileDir)
if ([string]::IsNullOrWhiteSpace($PackagePath)) {
    $PackagePath = Join-Path $Root 'dist\foo_ui_glasswave.fb2k-component'
}
else {
    $PackagePath = [IO.Path]::GetFullPath($PackagePath)
}

$targetExe = Join-Path $InstallDir 'foobar2000.exe'
if (-not (Test-Path -LiteralPath $targetExe -PathType Leaf)) {
    throw "Production foobar2000 executable not found: $targetExe"
}

$running = @(
    Get-Process foobar2000 -ErrorAction SilentlyContinue |
        Where-Object {
            try {
                [string]::Equals([IO.Path]::GetFullPath($_.Path), $targetExe, [StringComparison]::OrdinalIgnoreCase)
            }
            catch { $false }
        }
)
if ($running.Count -gt 0) {
    throw "Production foobar2000 ($targetExe) is currently running. Close it first before updating."
}

if ($Build -or -not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'package.ps1') -OutputPath $PackagePath
    if ($LASTEXITCODE -ne 0) {
        throw "package.ps1 failed with exit code $LASTEXITCODE."
    }
}

# 1. Cleanly install foo_ui_glasswave component from the packaged .fb2k-component
$componentDir = Join-Path $ProfileDir 'user-components-x64\foo_ui_glasswave'
if (Test-Path -LiteralPath $componentDir) {
    Remove-Item -LiteralPath $componentDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $componentDir | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($PackagePath, $componentDir)
Write-Host "Installed component from $PackagePath -> $componentDir"

if (-not $SkipLocalAssets) {
    $glasswaveDir = Join-Path $ProfileDir 'glasswave'
    New-Item -ItemType Directory -Force -Path $glasswaveDir | Out-Null

    # 2. Owner-supplied Win7 artwork (ADR 0007)
    $win7Sources = @{
        'wmploc.dll'    = '.local\refs\wmploc.dll'
        'ehres.dll'     = '.local\refs\ehres.dll'
        'aero.msstyles' = '.local\refs\Aero\aero.msstyles'
    }
    foreach ($name in $win7Sources.Keys) {
        $src = Join-Path $Root $win7Sources[$name]
        if (Test-Path -LiteralPath $src -PathType Leaf) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $glasswaveDir $name) -Force
            Write-Host "Synced Win7 artwork: $name"
        }
    }

    # 3. Owner-supplied Apple Music Android .so libraries (rootfs/system/lib64)
    $appleRootfsSrc = Join-Path $Root '.local\applemusic\windows\runtime\rootfs\system\lib64'
    if (Test-Path -LiteralPath $appleRootfsSrc -PathType Container) {
        $appleRootfsDst = Join-Path $glasswaveDir 'applemusic\runtime\rootfs\system\lib64'
        New-Item -ItemType Directory -Force -Path $appleRootfsDst | Out-Null
        $soFiles = @(Get-ChildItem -LiteralPath $appleRootfsSrc -Filter '*.so' -File)
        foreach ($so in $soFiles) {
            Copy-Item -LiteralPath $so.FullName -Destination (Join-Path $appleRootfsDst $so.Name) -Force
        }
        Write-Host "Synced $($soFiles.Count) Apple Music Android .so libraries -> $appleRootfsDst"
    }

    # 4. Owner-supplied local lyric scripts (ESLyric built-ins + community ESLyric-LyricsSource)
    $eslyricSrc = Join-Path $Root '.local\refs\lyrics\eslyric-pkg\emb_5923208\scripts'
    if (Test-Path -LiteralPath $eslyricSrc -PathType Container) {
        $eslyricDst = Join-Path $ProfileDir 'eslyric-data\scripts'
        New-Item -ItemType Directory -Force -Path $eslyricDst | Out-Null
        Copy-Item -Path (Join-Path $eslyricSrc '*') -Destination $eslyricDst -Recurse -Force
        Write-Host "Synced ESLyric scripts -> $eslyricDst"
    }
    $communitySrc = Join-Path $Root '.local\refs\lyrics\ESLyric-LyricsSource\current'
    $communityDst = Join-Path $glasswaveDir 'lyrics\scripts'
    foreach ($format in 'qrc', 'yrc', 'krc') {
        foreach ($kind in 'searcher', 'parser', 'lib') {
            $source = Join-Path $communitySrc "$format\$kind"
            if (-not (Test-Path -LiteralPath $source -PathType Container)) { continue }
            New-Item -ItemType Directory -Force -Path $communityDst | Out-Null
            Copy-Item -LiteralPath $source -Destination $communityDst -Recurse -Force
        }
    }
    if (Test-Path -LiteralPath $communityDst -PathType Container) {
        Write-Host "Synced community lyric scripts -> $communityDst"
    }
}

Write-Host "Done. Existing accounts, playlists, config.sqlite and caches were preserved."
