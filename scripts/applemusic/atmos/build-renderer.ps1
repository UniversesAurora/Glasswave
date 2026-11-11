#Requires -Version 7.2

[CmdletBinding()]
param(
    [string]$Ec3Path,
    [switch]$SkipTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$adapterRoot = Join-Path $repoRoot 'third_party\glasswave-cavern'
$pinPath = Join-Path $adapterRoot 'source-pin.json'
$patchPath = Join-Path $repoRoot 'scripts\applemusic\atmos\cavern-buffer-drain.patch'
$cavernParent = Join-Path $repoRoot '.local\applemusic\renderers\cavern'
$outputRoot = Join-Path $repoRoot '.local\applemusic\renderers\build\win-x64'
$probeOutput = Join-Path $outputRoot 'probe'

if (-not [string]::IsNullOrWhiteSpace($Ec3Path)) {
    $Ec3Path = [IO.Path]::GetFullPath($Ec3Path)
}

if (-not (Test-Path -LiteralPath $pinPath -PathType Leaf)) {
    throw "Cavern source pin is missing: $pinPath"
}
if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf)) {
    throw "Cavern adapter patch is missing: $patchPath"
}
$pin = Get-Content -LiteralPath $pinPath -Raw | ConvertFrom-Json -ErrorAction Stop
$cavernRoot = Join-Path $cavernParent ([string]$pin.commit)

function Invoke-Dotnet {
    param([Parameter(Mandatory)][string[]]$Arguments)
    & dotnet @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet failed with exit code $LASTEXITCODE"
    }
}

function Assert-DotnetSdkPin {
    $line = (& dotnet --version).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to read the selected .NET SDK version.'
    }
    if (-not [string]::Equals($line, [string]$pin.dotnetSdk, [StringComparison]::Ordinal)) {
        throw "The Cavern build requires .NET SDK $($pin.dotnetSdk); selected SDK is $line."
    }
}

Push-Location $adapterRoot
try {
    Assert-DotnetSdkPin

    if (-not (Test-Path -LiteralPath (Join-Path $cavernRoot '.git'))) {
        New-Item -ItemType Directory -Force -Path $cavernParent | Out-Null
        & git clone --filter=blob:none --no-checkout ([string]$pin.repository) $cavernRoot
        if ($LASTEXITCODE -ne 0) { throw 'Unable to clone the pinned Cavern source.' }
        & git -C $cavernRoot fetch --depth 1 origin ([string]$pin.commit)
        if ($LASTEXITCODE -ne 0) { throw 'Unable to fetch the pinned Cavern commit.' }
        & git -C $cavernRoot checkout --detach ([string]$pin.commit)
        if ($LASTEXITCODE -ne 0) { throw 'Unable to check out the pinned Cavern commit.' }
    }
    $actual = (& git -C $cavernRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or
        -not [string]::Equals($actual, [string]$pin.commit, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Cavern checkout does not match the source pin: $cavernRoot"
    }
    & git -C $cavernRoot apply --reverse --check $patchPath 2>$null
    $patchApplied = $LASTEXITCODE -eq 0
    if (-not $patchApplied) {
        & git -C $cavernRoot apply --check $patchPath
        if ($LASTEXITCODE -ne 0) {
            throw 'The pinned Cavern source does not accept the buffer-drain adapter patch.'
        }
        & git -C $cavernRoot apply $patchPath
        if ($LASTEXITCODE -ne 0) {
            throw 'Unable to apply the Cavern buffer-drain adapter patch.'
        }
    }

    New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
    Invoke-Dotnet @(
        'publish',
        (Join-Path $adapterRoot 'Glasswave.Cavern.Native.csproj'),
        '-c', 'Release',
        '-r', 'win-x64',
        '--self-contained', 'true',
        "-p:CavernRoot=$cavernRoot",
        '-o', $outputRoot
    )

    Copy-Item -LiteralPath (Join-Path $cavernRoot 'LICENSE.md') `
              -Destination (Join-Path $outputRoot 'Cavern-LICENSE.md') -Force

    if (-not $SkipTest) {
        if ([string]::IsNullOrWhiteSpace($Ec3Path)) {
            $defaultFixture = Join-Path $repoRoot '.local\tests\applemusic-atmos-source.ec3'
            if (Test-Path -LiteralPath $defaultFixture -PathType Leaf) {
                $Ec3Path = $defaultFixture
            }
        }
        if ([string]::IsNullOrWhiteSpace($Ec3Path) -or
            -not (Test-Path -LiteralPath $Ec3Path -PathType Leaf)) {
            throw 'Pass -Ec3Path or create .local/tests/applemusic-atmos-source.ec3.'
        }
        Invoke-Dotnet @(
            'build',
            (Join-Path $adapterRoot 'test\Glasswave.Cavern.Probe.csproj'),
            '-c', 'Release',
            '-o', $probeOutput
        )
        $probeDll = Join-Path $probeOutput 'Glasswave.Cavern.Probe.dll'
        $rendererDll = Join-Path $outputRoot 'glasswave_cavern.dll'

        function Invoke-RendererProbe {
            param(
                [Parameter(Mandatory)][int]$Layout,
                [Parameter(Mandatory)][int]$Sessions,
                [Parameter(Mandatory)][string]$FixturePath,
                [int]$Mode = 0,
                [Nullable[int]]$LayoutB,
                [Nullable[long]]$ExpectedFrames
            )

            $probeArguments = @(
                $probeDll,
                '--dll', $rendererDll,
                '--ec3', $FixturePath,
                '--mode', $Mode,
                '--layout', $Layout,
                '--sessions', $Sessions
            )
            if ($null -ne $LayoutB) {
                $probeArguments += @('--layout-b', $LayoutB)
            }
            if ($null -ne $ExpectedFrames) {
                $probeArguments += @('--expected-frames', $ExpectedFrames)
            }
            $json = & dotnet @probeArguments
            if ($LASTEXITCODE -ne 0) {
                throw "The real EC-3 probe failed with exit code $LASTEXITCODE"
            }
            Write-Host $json
            return $json | ConvertFrom-Json -ErrorAction Stop
        }

        # The raw EC-3 fixture has 5,835 complete 1,536-frame packets. Do not
        # copy the smaller fMP4 presentation duration or drop a priming frame.
        $longFrames = 8962560
        $baseline20 = Invoke-RendererProbe -Layout 0 -Sessions 1 `
            -FixturePath $Ec3Path -ExpectedFrames $longFrames
        $baseline51 = Invoke-RendererProbe -Layout 1 -Sessions 1 `
            -FixturePath $Ec3Path -ExpectedFrames $longFrames
        $baseline71 = Invoke-RendererProbe -Layout 2 -Sessions 1 `
            -FixturePath $Ec3Path -ExpectedFrames $longFrames
        $baselineHeadphones = Invoke-RendererProbe -Layout 0 -Sessions 1 -Mode 1 `
            -FixturePath $Ec3Path -ExpectedFrames $longFrames
        $interleaved20 = Invoke-RendererProbe -Layout 0 -Sessions 2 `
            -FixturePath $Ec3Path -ExpectedFrames $longFrames
        $interleavedHeadphones = Invoke-RendererProbe -Layout 0 -Sessions 2 -Mode 1 `
            -FixturePath $Ec3Path -ExpectedFrames $longFrames
        $mixed = Invoke-RendererProbe -Layout 0 -LayoutB 1 -Sessions 2 `
            -FixturePath $Ec3Path -ExpectedFrames $longFrames

        $shortFixturePath = Join-Path $repoRoot '.local\tests\atmos-48-progressive.ec3'
        if (-not (Test-Path -LiteralPath $shortFixturePath -PathType Leaf)) {
            throw "The short EOF regression fixture is missing: $shortFixturePath"
        }
        $shortEof = Invoke-RendererProbe -Layout 0 -Sessions 1 `
            -FixturePath $shortFixturePath -ExpectedFrames 144384

        $baseline20Hash = [string]@($baseline20.runs)[0].pcmSha256
        $baseline51Hash = [string]@($baseline51.runs)[0].pcmSha256
        $baseline71Hash = [string]@($baseline71.runs)[0].pcmSha256
        $headphoneHash = [string]@($baselineHeadphones.runs)[0].pcmSha256
        foreach ($pcmHash in @($baseline20Hash, $baseline51Hash, $baseline71Hash, $headphoneHash)) {
            if ($pcmHash -notmatch '^[0-9a-fA-F]{64}$') {
                throw 'The EC-3 probe did not return a valid PCM SHA-256 digest.'
            }
        }
        $interleaved20Runs = @($interleaved20.runs)
        if ($interleaved20Runs.Count -ne 2 -or
            $interleaved20Runs.Where({ $_.pcmSha256 -ne $baseline20Hash }).Count -ne 0) {
            throw 'Interleaved 2.0 PCM does not exactly match the standalone baseline.'
        }

        $interleavedHeadphoneRuns = @($interleavedHeadphones.runs)
        if ($interleavedHeadphoneRuns.Count -ne 2 -or
            $interleavedHeadphoneRuns.Where({ $_.pcmSha256 -ne $headphoneHash }).Count -ne 0) {
            throw 'Interleaved headphone PCM does not exactly match the standalone baseline.'
        }

        $mixedRuns = @($mixed.runs)
        if ($mixedRuns.Count -ne 2 -or
            [int]$mixedRuns[0].layout -ne 0 -or
            [int]$mixedRuns[1].layout -ne 1 -or
            $mixedRuns[0].pcmSha256 -ne $baseline20Hash -or
            $mixedRuns[1].pcmSha256 -ne $baseline51Hash) {
            throw 'Mixed-layout PCM does not exactly match each standalone baseline.'
        }

        Write-Host ('Verified PCM SHA-256 baselines: 2.0={0}; 5.1={1}; 7.1={2}; headphones={3}' -f
            $baseline20Hash,
            $baseline51Hash,
            $baseline71Hash,
            [string]@($baselineHeadphones.runs)[0].pcmSha256)
    }

    $rendererDll = Join-Path $outputRoot 'glasswave_cavern.dll'
    if (-not (Test-Path -LiteralPath $rendererDll -PathType Leaf)) {
        throw "Cavern renderer was not published: $rendererDll"
    }
    Write-Host "Atmos renderer ready: $rendererDll"
} finally {
    Pop-Location
}
