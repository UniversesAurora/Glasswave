#Requires -Version 7.2
<#
.SYNOPSIS
    Build a clean, publicly redistributable foo_ui_glasswave.fb2k-component package.

.DESCRIPTION
    Packages Glasswave's own component DLL, Cavern Atmos renderer, UI translations,
    MIT-licensed lyric scripts, and the open-source Apple Music native Windows bridge
    binaries (engine.exe + libhybris-win + MinGW runtime + licenses) into a standard
    foobar2000 .fb2k-component (ZIP) archive.

    Strictly excludes Microsoft Win7 artwork (ADR 0005/0007), Apple proprietary Android
    .so libraries, ESLyric proprietary scripts, unrelated third-party foobar2000 plugins,
    and all personal accounts, playlists, or caches.

.EXAMPLE
    pwsh -File scripts/package.ps1
    pwsh -File scripts/package.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$OutputPath,

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $Root 'dist\foo_ui_glasswave.fb2k-component'
}
else {
    $OutputPath = [IO.Path]::GetFullPath($OutputPath)
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "build.ps1 failed with exit code $LASTEXITCODE."
    }
}

$dllPath = Join-Path $Root "build\x64\$Configuration\foo_ui_glasswave.dll"
if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
    throw "Missing built component: $dllPath"
}

$cavernDir = Join-Path $Root '.local\applemusic\renderers\build\win-x64'
$cavernDll = Join-Path $cavernDir 'glasswave_cavern.dll'
$cavernLicense = Join-Path $cavernDir 'Cavern-LICENSE.md'
$cavernNotice = Join-Path $Root 'third_party\glasswave-cavern\NOTICE.md'
foreach ($required in @($cavernDll, $cavernLicense, $cavernNotice)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing Atmos renderer artifact: $required (run scripts/applemusic/atmos/build-renderer.ps1)"
    }
}

$appleRuntimeDir = Join-Path $Root '.local\applemusic\windows\runtime'
$appleOpenSourceFiles = @(
    'engine.exe'
    'libdrm_client.dll'
    'libhybris-core.dll'
    'libgcc_s_seh-1.dll'
    'libstdc++-6.dll'
    'libwinpthread-1.dll'
    'hybris-linker\q.so'
)
foreach ($rel in $appleOpenSourceFiles) {
    $full = Join-Path $appleRuntimeDir $rel
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "Missing Apple Music open-source runtime binary: $full (run scripts/setup-applemusic.ps1)"
    }
}

$stageDir = Join-Path ([IO.Path]::GetTempPath()) ("glasswave-pkg-" + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    # 1. Core component & Cavern Atmos renderer
    Copy-Item -LiteralPath $dllPath -Destination (Join-Path $stageDir 'foo_ui_glasswave.dll') -Force
    Copy-Item -LiteralPath $cavernDll -Destination (Join-Path $stageDir 'glasswave_cavern.dll') -Force
    Copy-Item -LiteralPath $cavernLicense -Destination (Join-Path $stageDir 'Cavern-LICENSE.md') -Force
    Copy-Item -LiteralPath $cavernNotice -Destination (Join-Path $stageDir 'Cavern-NOTICE.md') -Force

    # 2. UI translations (lang/*.json)
    $langSrc = Join-Path $Root 'lang'
    $langDst = Join-Path $stageDir 'lang'
    New-Item -ItemType Directory -Force -Path $langDst | Out-Null
    Get-ChildItem -LiteralPath $langSrc -Filter '*.json' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $langDst $_.Name) -Force
    }

    # 3. Glasswave-authored MIT lyric scripts (scripts/lyrics/**)
    $lyricsSrc = Join-Path $Root 'scripts\lyrics'
    if (Test-Path -LiteralPath $lyricsSrc -PathType Container) {
        $lyricsDst = Join-Path $stageDir 'lyrics\scripts'
        New-Item -ItemType Directory -Force -Path $lyricsDst | Out-Null
        Copy-Item -Path (Join-Path $lyricsSrc '*') -Destination $lyricsDst -Recurse -Force
    }

    # 4. Open-source Apple Music native bridge binaries + licenses (no Apple Android .so)
    $appleDst = Join-Path $stageDir 'applemusic\runtime'
    foreach ($rel in $appleOpenSourceFiles) {
        $src = Join-Path $appleRuntimeDir $rel
        $dst = Join-Path $appleDst $rel
        $parent = Split-Path -Parent $dst
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }
    $licensesSrc = Join-Path $Root 'scripts\applemusic\licenses'
    if (Test-Path -LiteralPath $licensesSrc -PathType Container) {
        $licensesDst = Join-Path $appleDst 'licenses'
        New-Item -ItemType Directory -Force -Path $licensesDst | Out-Null
        Get-ChildItem -LiteralPath $licensesSrc -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $licensesDst $_.Name) -Force
        }
    }

    # 5. Strict safety audit before archiving: reject proprietary / personal / unrelated files
    $forbiddenNames = @(
        'wmploc.dll', 'ehres.dll', 'aero.msstyles',
        'libandroidappmusic.so', 'libCoreFP.so', 'libCoreADI.so',
        'bridge.json', 'config.yaml', 'session.dat', 'config.sqlite', 'index.sqlite3'
    )
    $stagedFiles = Get-ChildItem -LiteralPath $stageDir -Recurse -File
    foreach ($f in $stagedFiles) {
        $relPath = $f.FullName.Substring($stageDir.Length + 1).Replace('\', '/')
        if ($forbiddenNames -contains $f.Name -or
            $relPath -match '(^|/)rootfs/' -or
            $relPath -match '(^|/)widevine/' -or
            $relPath -match '(^|/)mpl_db/' -or
            $f.Name -like '*.log' -or
            $f.Name -like '*.pdb') {
            throw "Packaging safety check failed: forbidden file staged: $relPath"
        }
    }

    $outParent = Split-Path -Parent $OutputPath
    New-Item -ItemType Directory -Force -Path $outParent | Out-Null
    if (Test-Path -LiteralPath $OutputPath) {
        Remove-Item -LiteralPath $OutputPath -Force
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($OutputPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $stagedFiles) {
            $entryName = $f.FullName.Substring($stageDir.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $f.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal)
        }
    }
    finally {
        $zip.Dispose()
    }

    $sizeMiB = [math]::Round((Get-Item -LiteralPath $OutputPath).Length / 1MB, 2)
    Write-Host "Packaged $($stagedFiles.Count) files ($sizeMiB MiB) -> $OutputPath"
}
finally {
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
