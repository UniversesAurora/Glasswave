#Requires -Version 7.2
<#
.SYNOPSIS
    Deploy the built component into the .local/fb2k-dev portable instance and start it.

.DESCRIPTION
    Only touches the repository's own dev instance (.local/fb2k-dev), never any other
    foobar2000 installation on the machine. A running dev instance is asked to close
    first (-Kill forces it).

.EXAMPLE
    pwsh -File scripts/run-dev.ps1               # deploy Release|x64 and start
    pwsh -File scripts/run-dev.ps1 -NoLaunch     # deploy only
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [switch]$NoLaunch,
    [switch]$Kill
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
if (-not (Test-Path -LiteralPath $Exe)) { throw 'Dev instance missing - run scripts/bootstrap.ps1 first' }

$built = Join-Path $Root "build/x64/$Configuration"
$dll = Join-Path $built 'foo_ui_glasswave.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Missing $dll - run scripts/build.ps1 -Configuration $Configuration" }

# Only processes whose image is this instance's foobar2000.exe.
$procs = Get-Process foobar2000 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
foreach ($p in $procs) {
    if ($Kill) { $p.Kill() } else { [void]$p.CloseMainWindow() }
    if (-not $p.WaitForExit(8000)) { $p.Kill(); [void]$p.WaitForExit(3000) }
}

# foobar2000 2.x portable mode: 64-bit components live in profile/user-components-x64/<name>/
$dest = Join-Path $Instance 'profile/user-components-x64/foo_ui_glasswave'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -LiteralPath $dll -Destination $dest -Force
$pdb = Join-Path $built 'foo_ui_glasswave.pdb'
if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $dest -Force }
Write-Host "Deployed $Configuration build to $dest"

# Native object renderer, built separately from its pinned source with no system installation.
$atmosRenderer = Join-Path $Root '.local/applemusic/renderers/build/win-x64/glasswave_cavern.dll'
if (Test-Path -LiteralPath $atmosRenderer -PathType Leaf) {
    Copy-Item -LiteralPath $atmosRenderer -Destination (Join-Path $dest 'glasswave_cavern.dll') -Force
    $atmosLicense = Join-Path (Split-Path $atmosRenderer -Parent) 'Cavern-LICENSE.md'
    if (Test-Path -LiteralPath $atmosLicense -PathType Leaf) {
        Copy-Item -LiteralPath $atmosLicense -Destination (Join-Path $dest 'Cavern-LICENSE.md') -Force
    }
    Copy-Item -LiteralPath (Join-Path $Root 'third_party/glasswave-cavern/NOTICE.md') `
        -Destination (Join-Path $dest 'Cavern-NOTICE.md') -Force
}

# Original Win7 artwork (ADR 0007): never committed; copied from the owner's local
# references into the dev profile, where Glasswave looks for it at run time.
$art = Join-Path $Instance 'profile/glasswave'
$sources = @{
    'wmploc.dll'    = '.local/refs/wmploc.dll'
    'ehres.dll'     = '.local/refs/ehres.dll'
    'aero.msstyles' = '.local/refs/Aero/aero.msstyles'
}
foreach ($name in $sources.Keys) {
    $src = Join-Path $Root $sources[$name]
    if (-not (Test-Path -LiteralPath $src)) { continue }
    New-Item -ItemType Directory -Force -Path $art | Out-Null
    $dst = Join-Path $art $name
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Host "Copied Win7 artwork $name"
}

# UI translations (lang/<code>.json, ours to ship) next to the artwork.
$langSrc = Join-Path $Root 'lang'
if (Test-Path -LiteralPath $langSrc) {
    $langDst = Join-Path $art 'lang'
    New-Item -ItemType Directory -Force -Path $langDst | Out-Null
    Get-ChildItem -LiteralPath $langSrc -Filter '*.json' | Copy-Item -Destination $langDst -Force
}

# Original lyric source trees supplied locally by the owner. Keep every script and
# dependency unchanged, scoped to the dev profile and excluded from the repository.
$eslyricScripts = Join-Path $Root '.local/refs/lyrics/eslyric-pkg/emb_5923208/scripts'
if (Test-Path -LiteralPath $eslyricScripts) {
    $eslyricTarget = Join-Path $Instance 'profile/eslyric-data/scripts'
    New-Item -ItemType Directory -Force -Path $eslyricTarget | Out-Null
    Copy-Item -Path (Join-Path $eslyricScripts '*') -Destination $eslyricTarget -Recurse -Force
}
$communityScripts = Join-Path $Root '.local/refs/lyrics/ESLyric-LyricsSource/current'
$communityTarget = Join-Path $art 'lyrics/scripts'
foreach ($format in 'qrc', 'yrc', 'krc') {
    foreach ($kind in 'searcher', 'parser', 'lib') {
        $source = Join-Path $communityScripts "$format/$kind"
        if (-not (Test-Path -LiteralPath $source)) { continue }
        New-Item -ItemType Directory -Force -Path $communityTarget | Out-Null
        Copy-Item -LiteralPath $source -Destination $communityTarget -Recurse -Force
    }
}

# Glasswave-authored MIT lyric sources.
$lyricSource = Join-Path $Root 'scripts/lyrics'
$lyricTarget = Join-Path $art 'lyrics/scripts'
if (Test-Path -LiteralPath $lyricSource) {
    New-Item -ItemType Directory -Force -Path $lyricTarget | Out-Null
    Copy-Item -Path (Join-Path $lyricSource '*') -Destination $lyricTarget -Recurse -Force
}

if (-not $NoLaunch) {
    # foobar2000 owns the lifecycle of the installed production runtime. Fixture
    # bridges and explicitly selected alternate endpoints remain unchanged.
    . (Join-Path $PSScriptRoot 'applemusic/windows/common.ps1')
    $apple = Get-GlasswavePaths
    $appleInstalled = Test-GlasswaveRuntimeComplete
    $prepareAppleBridge = $false
    if ($appleInstalled) {
        if (-not (Test-Path -LiteralPath $apple.BridgeConfigPath -PathType Leaf)) {
            $prepareAppleBridge = $true
        }
        elseif (-not (Test-GlasswaveBridgeConfig -Port 20025)) {
            try {
                $appleConfig = Get-Content -LiteralPath $apple.BridgeConfigPath -Raw |
                    ConvertFrom-Json -ErrorAction Stop
                $prepareAppleBridge = $appleConfig.url -eq 'http://127.0.0.1:20025'
            }
            catch {
                Write-Warning 'Existing Apple Music bridge is not valid JSON and was left unchanged.'
            }
        }
    }
    if ($prepareAppleBridge) {
        Initialize-GlasswaveApiToken
        Write-GlasswaveBridgeConfig -Port 20025
        Write-Host 'Configured the app-managed native Apple Music runtime'
    }
    Start-Process -FilePath $Exe -WorkingDirectory $Instance
    Write-Host 'Dev instance started'
}
